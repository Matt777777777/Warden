#!/usr/bin/env python3
"""
WARDEN - watch the on-device AI live (firmware v0.4).

    python tools/watch_ai.py

Hides the 100 Hz sensor stream and shows only what the crate decides: the model's
verdict every 0.5 s (#M lines) and the events it logs (#E lines). Ctrl+C to stop.
Close the Arduino Serial Monitor first: only one program can use the port.
"""

import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from record import find_port  # noqa: E402

CLASSES = ["still", "handling", "drop", "tamper"]
RED, BOLD, DIM, END = "\033[91m", "\033[1m", "\033[2m", "\033[0m"


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else find_port()
    print(f"Connecting to {port} (the board restarts, keep the crate still for 2 s) ...")
    us_max = 0
    with serial.Serial(port, 115200, timeout=1) as ser:
        start = time.time()
        while True:
            line = ser.readline().decode("ascii", errors="ignore").strip()
            if line.startswith("#M,"):
                try:
                    cls, *votes, lid, us = map(int, line[3:].split(","))
                except ValueError:
                    continue
                us_max = max(us_max, us)
                name = CLASSES[cls] if cls < len(CLASSES) else "?"
                tamper = votes[3]
                bar = "#" * tamper + "." * (10 - tamper)
                color = RED if tamper >= 8 and not lid else ""
                lid_txt = "lid OPEN (tamper ignored)" if lid else "lid closed"
                print(f"{time.time() - start:7.1f}s  {color}{name:<9}{END} tamper [{color}{bar}{END}] "
                      f"{tamper:2d}/10  {DIM}{lid_txt}, {us / 1000:.1f} ms (max {us_max / 1000:.1f}){END}")
            elif line.startswith("#E "):
                txt = line[3:]
                print(f"{BOLD}{RED if 'ALERT' in txt else ''}  EVENT  {txt}{END}")
            elif line.startswith("# "):
                print(f"{DIM}  board: {line[2:]}{END}")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
    except serial.SerialException as e:
        sys.exit(f"Serial port problem: {e}\nClose the Arduino Serial Monitor (or quit the IDE) and retry.")
