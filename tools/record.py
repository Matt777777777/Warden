#!/usr/bin/env python3
"""
WARDEN - record labelled sessions of sensor data for the training dataset.

Usage examples
    python tools/record.py --list                         # show serial ports
    python tools/record.py --label still --seconds 20
    python tools/record.py --label drop --seconds 20 --repeat 15 --notes "carpet"
    python tools/record.py --summary                      # what has been collected so far

Each session creates  data/raw/<label>_<YYYYmmdd-HHMMSS>.csv
With --repeat, sessions follow each other: Enter = next, d = discard the last one, q = quit.
"""

import argparse
import csv
import sys
import time
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports

LABELS = ["still", "carry", "vehicle", "drop", "tamper", "flip", "open"]
TARGET_SESSIONS = {"still": 15, "carry": 15, "vehicle": 10, "drop": 15, "tamper": 15, "flip": 15, "open": 15}
HEADER = ["t_ms", "ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps", "light"]
ROOT = Path(__file__).resolve().parent.parent


def find_port():
    """Guess which serial port is the Arduino."""
    keywords = ("arduino", "usbmodem", "usbserial", "ch340", "wchusb", "ttyacm", "ttyusb")
    found = []
    for p in list_ports.comports():
        text = f"{p.device} {p.description} {p.manufacturer or ''}".lower()
        if any(k in text for k in keywords):
            found.append(p.device)
    if not found:
        sys.exit("No Arduino found. Is the USB cable plugged in? Try --list, then --port <name>.")
    if len(found) > 1:
        print(f"Several candidates {found}, using {found[0]} (override with --port).")
    return found[0]


def wait_for_header(ser, timeout_s=15):
    """Opening the port resets the Arduino: wait until it prints the CSV header."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode("ascii", errors="ignore").strip()
        if not line:
            continue
        if line.startswith("#"):
            print("  board:", line[1:].strip())
            if "ERROR" in line:
                sys.exit("The board reported an error, fix it and retry.")
        elif line.split(",") == HEADER or parse(line) is not None:
            return  # header seen, or the board is already streaming valid data
    sys.exit("No data header received. Is the imu_stream sketch uploaded? Is baud 115200?")


def parse(line):
    """Turn 'a,b,c,...' into a list of 8 numbers, or None if the line is damaged."""
    parts = line.split(",")
    if len(parts) != len(HEADER):
        return None
    try:
        return [int(parts[0])] + [float(x) for x in parts[1:7]] + [int(parts[7])]
    except ValueError:
        return None


def bar(value, scale=4.0, width=30):
    n = min(width, int(abs(value) / scale * width))
    return "#" * n + " " * (width - n)


def record_session(ser, label, seconds, out_dir, notes, number=None):
    """Countdown, record one session, save it. Returns the saved path (or None)."""
    title = f"'{label}' for {seconds:.0f} s" + (f"  (session {number})" if number else "")
    print(f"\nReady to record {title}.")
    print("Start your camera now. On GO, tap the crate twice (sync mark for the video).")
    for i in (3, 2, 1):
        print(f"  {i}...", end="\a\n", flush=True)
        time.sleep(1)
    print("  GO!\a")

    # Drop the samples that piled up during the countdown. reset_input_buffer()
    # alone is not reliable on macOS, so also read and discard until caught up.
    ser.reset_input_buffer()
    drain_deadline = time.time() + 2
    while ser.in_waiting > 200 and time.time() < drain_deadline:
        ser.readline()
    rows, bad = [], 0
    start = time.time()
    while time.time() - start < seconds:
        line = ser.readline().decode("ascii", errors="ignore").strip()
        if not line or line.startswith("#"):
            continue
        values = parse(line)
        if values is None:
            bad += 1
            continue
        rows.append(values)
        if len(rows) % 10 == 0:       # live feedback, 10 times per second
            mag = (values[1] ** 2 + values[2] ** 2 + values[3] ** 2) ** 0.5
            left = seconds - (time.time() - start)
            print(f"\r  |a|={mag:5.2f} g [{bar(mag)}] light={values[7]:4d}  {left:4.1f}s left",
                  end="", flush=True)
    print("\a")

    if not rows:
        print("No samples recorded. Check the cable and the sketch.")
        return None

    # Safety net: keep only the last N seconds measured by the board's own clock,
    # so no pre-GO sample can end up carrying this label.
    t_end = rows[-1][0]
    rows = [r for r in rows if r[0] > t_end - seconds * 1000]

    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = out_dir / f"{label}_{stamp}.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(HEADER + ["label"])
        for r in rows:
            w.writerow(r + [label])

    duration = (rows[-1][0] - rows[0][0]) / 1000 or 1
    rate = (len(rows) - 1) / duration
    with open(out_dir / "sessions_log.csv", "a", newline="") as f:
        csv.writer(f).writerow([stamp, label, len(rows), f"{rate:.1f}", path.name, notes])

    print(f"Saved {len(rows)} samples ({rate:.1f} Hz, {bad} damaged lines skipped) -> {path.name}")
    if rate < 90:
        print("Warning: sample rate below 90 Hz. Close the Arduino Serial Monitor and retry.")
    return path


def summary(out_dir):
    """How many sessions per label, versus the targets of the collection protocol."""
    counts, seconds = {}, {}
    for f in sorted(out_dir.glob("*_*.csv")):
        label = f.name.split("_")[0]
        if label not in LABELS:
            continue
        with open(f) as fh:
            n = sum(1 for _ in fh) - 1
        counts[label] = counts.get(label, 0) + 1
        seconds[label] = seconds.get(label, 0) + n / 100
    print(f"\nDataset in {out_dir}")
    print(f"{'label':<9}{'sessions':>9}{'target':>8}{'minutes':>9}  progress")
    total = 0
    for label in LABELS:
        c, t = counts.get(label, 0), TARGET_SESSIONS[label]
        total += c
        done = min(10, round(10 * c / t))
        print(f"{label:<9}{c:>9}{t:>8}{seconds.get(label, 0) / 60:>9.1f}  [{'#' * done}{'.' * (10 - done)}]")
    print(f"{'total':<9}{total:>9}{sum(TARGET_SESSIONS.values()):>8}{sum(seconds.values()) / 60:>9.1f}\n")


def main():
    ap = argparse.ArgumentParser(description="Record labelled WARDEN sessions.")
    ap.add_argument("--label", choices=LABELS, help="what you are about to do with the crate")
    ap.add_argument("--seconds", type=float, default=20, help="recording length (default 20)")
    ap.add_argument("--repeat", type=int, default=1, help="record several sessions in a row")
    ap.add_argument("--port", help="serial port, auto-detected if omitted")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default=str(ROOT / "data" / "raw"), help="output folder")
    ap.add_argument("--notes", default="", help="free text saved in the session log")
    ap.add_argument("--list", action="store_true", help="list serial ports and exit")
    ap.add_argument("--summary", action="store_true", help="show what has been collected and exit")
    args = ap.parse_args()
    out_dir = Path(args.out)

    if args.list:
        for p in list_ports.comports():
            print(f"{p.device:30s} {p.description}")
        return
    if args.summary:
        summary(out_dir)
        return
    if not args.label:
        ap.error("--label is required (one of: " + ", ".join(LABELS) + ")")

    port = args.port or find_port()
    print(f"Connecting to {port} ...")
    try:
        ser = serial.Serial(port, args.baud, timeout=1)
    except serial.SerialException as e:
        if "busy" in str(e).lower():
            sys.exit("The port is busy: another program is talking to the Arduino.\n"
                     "Close the Serial Monitor tab and the Serial Plotter in the Arduino IDE "
                     "(or quit the IDE with Cmd+Q), then run this command again.")
        sys.exit(f"Could not open {port}: {e}")
    with ser:
        wait_for_header(ser)
        done, last = 0, None
        while done < args.repeat:
            path = record_session(ser, args.label, args.seconds, out_dir, args.notes,
                                  number=f"{done + 1}/{args.repeat}" if args.repeat > 1 else None)
            if path:
                done += 1
                last = path
            if done >= args.repeat:
                break
            answer = input("Enter = next session, d = discard the last one, q = quit: ").strip().lower()
            if answer == "q":
                break
            if answer == "d" and last and last.exists():
                last.unlink()
                with open(out_dir / "sessions_log.csv", "a", newline="") as f:
                    csv.writer(f).writerow([datetime.now().strftime("%Y%m%d-%H%M%S"), args.label, 0, "",
                                            last.name, "DISCARDED"])
                print(f"Discarded {last.name}")
                done -= 1
                last = None
    summary(out_dir)


if __name__ == "__main__":
    main()
