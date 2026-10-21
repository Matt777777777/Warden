#!/usr/bin/env python3
"""
WARDEN - Mac relay: lets the phone app talk to the crate through the Mac's Wi-Fi.

    crate --USB--> Mac (this script) --Wi-Fi--> iPhone (Safari)

The crate stays plugged into the Mac. This script reads what the crate says on USB,
serves the phone app, and relays the crate's messages to the phone in real time
(and the phone's commands back to the crate). No Bluetooth needed, works in Safari.

Usage
    python tools/bridge.py            # then open the address it prints on your iPhone
    python tools/bridge.py --port /dev/cu.usbmodem14201 --http 8000

The iPhone must be on the same Wi-Fi as the Mac. If macOS asks whether Python may
accept incoming connections, click Allow.
"""

import argparse
import queue
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parent.parent
APP_FILE = ROOT / "app" / "index.html"

clients = []                      # one queue per connected phone
clients_lock = threading.Lock()
ser = None
ser_lock = threading.Lock()


def find_port():
    keys = ("usbmodem", "usbserial", "ttyacm", "ttyusb", "arduino", "ch340", "wchusb")
    ports = [p.device for p in list_ports.comports()
             if any(k in f"{p.device} {p.description}".lower() for k in keys)]
    if not ports:
        sys.exit("No Arduino found. Is the crate plugged into the Mac?")
    return ports[0]


def local_ip():
    """The Mac's address on the Wi-Fi (what the iPhone must type)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))          # no packet is actually sent
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def broadcast(line):
    with clients_lock:
        for q in list(clients):
            try:
                q.put_nowait(line)
            except queue.Full:
                pass


def serial_reader():
    """Reads the crate forever and forwards its '#' messages (status, events, log)."""
    while True:
        try:
            raw = ser.readline()
        except serial.SerialException:
            print("\nCrate disconnected. Plug it back in and restart the bridge.")
            broadcast("#X,disconnected")
            return
        line = raw.decode("ascii", "ignore").strip()
        if not line.startswith("#"):
            continue                                   # 100 Hz sensor lines stay on the Mac
        if line.startswith(("#E", "#L,END", "# ERROR", "# WARNING")):
            print("  crate:", line)
        broadcast(line)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):                      # keep the terminal quiet
        pass

    def send_text(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(302)
            self.send_header("Location", "/app/")
            self.end_headers()
        elif self.path.startswith("/app"):
            self.send_text(200, APP_FILE.read_bytes(), "text/html; charset=utf-8")
        elif self.path == "/bridge-info":
            self.send_text(200, '{"bridge": "warden", "port": "%s"}' % ser.port, "application/json")
        elif self.path == "/events":
            self.stream_events()
        else:
            self.send_text(404, "not found")

    def stream_events(self):
        """Server-Sent Events: one long-lived response, one 'data:' line per crate message."""
        q = queue.Queue(maxsize=2000)
        with clients_lock:
            clients.append(q)
        print(f"  phone connected ({self.client_address[0]})")
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.wfile.write(b": connected\n\n")
            self.wfile.flush()
            while True:
                try:
                    line = q.get(timeout=15)
                    self.wfile.write(f"data: {line}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": keep-alive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            with clients_lock:
                if q in clients:
                    clients.remove(q)
            print(f"  phone disconnected ({self.client_address[0]})")

    def do_POST(self):
        if self.path != "/cmd":
            self.send_text(404, "not found")
            return
        length = int(self.headers.get("Content-Length", 0))
        cmd = self.rfile.read(min(length, 64)).decode("ascii", "ignore").strip()
        if not (cmd == "dump" or cmd.startswith("time ")):
            self.send_text(400, "command not allowed")      # the phone may not erase the log
            return
        with ser_lock:
            ser.write((cmd + "\n").encode())
        print("  phone ->", cmd)
        self.send_text(200, "ok")


def main():
    global ser
    ap = argparse.ArgumentParser(description="Relay between the WARDEN crate (USB) and the phone app (Wi-Fi).")
    ap.add_argument("--port", help="serial port of the crate (auto-detected if omitted)")
    ap.add_argument("--http", type=int, default=8000, help="web port for the phone (default 8000)")
    args = ap.parse_args()

    if not APP_FILE.exists():
        sys.exit(f"App not found at {APP_FILE}")
    port = args.port or find_port()
    try:
        ser = serial.Serial(port, 115200, timeout=1)
    except serial.SerialException as e:
        if "busy" in str(e).lower():
            sys.exit("The crate's port is busy: close the Arduino Serial Monitor (or record.py) and retry.")
        sys.exit(f"Could not open {port}: {e}")

    print(f"Crate on {port}. Waiting for it to start...")
    time.sleep(2)
    threading.Thread(target=serial_reader, daemon=True).start()

    server = ThreadingHTTPServer(("0.0.0.0", args.http), Handler)
    server.daemon_threads = True
    host = socket.gethostname()
    if not host.endswith(".local"):
        host += ".local"
    print("\nOn your iPhone (same Wi-Fi), open in Safari:")
    print(f"    http://{local_ip()}:{args.http}/app/")
    print(f"    (or http://{host}:{args.http}/app/)")
    print("Then tap 'Connect to crate'. Press Ctrl+C here to stop.\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nBridge stopped.")


if __name__ == "__main__":
    main()
