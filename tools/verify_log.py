#!/usr/bin/env python3
"""
WARDEN - read the black box log and check that nobody tampered with it.

Every record stored by the firmware carries a "chain" value: a CRC-16 fingerprint of
the previous chain value + this record's bytes. Change, delete or reorder a single
record and every fingerprint after it stops matching, like a numbered, glued logbook
with a torn-out page.

Usage
    python tools/verify_log.py                 # asks the board for its log over USB
    python tools/verify_log.py --file dump.txt # checks a saved dump instead
    python tools/verify_log.py --save dump.txt # reads the board and saves the raw dump

Note: CRC-16 detects accidental or naive edits. A determined attacker who knows the
algorithm could recompute it; a production version would use a keyed hash (HMAC)
with a secret stored in a secure element. That upgrade is on the roadmap.
"""

import argparse
import sys
import time
from datetime import datetime, timezone

ALERTS = {"OPEN !!", "DROP", "SHOCK", "FLIPPED", "BADGE ?", "TAMPER"}


def crc16(crc, data):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


EVENT_CODES = {"BOOT": 1, "OPEN OK": 2, "OPEN !!": 3, "CLOSED": 4, "DROP": 5, "SHOCK": 6,
               "FLIPPED": 7, "UPRIGHT": 8, "AUTH": 9, "INSPECT": 10, "BADGE ?": 11, "CLEARED": 12,
               "TAMPER": 13}


def read_from_board(port=None, baud=115200, timeout=20):
    import serial
    from serial.tools import list_ports
    if port is None:
        keys = ("usbmodem", "usbserial", "ttyacm", "ttyusb", "arduino", "ch340")
        ports = [p.device for p in list_ports.comports()
                 if any(k in f"{p.device} {p.description}".lower() for k in keys)]
        if not ports:
            sys.exit("No Arduino found. Plug it in, or pass --port.")
        port = ports[0]
    try:
        ser = serial.Serial(port, baud, timeout=1)
    except serial.SerialException as e:
        sys.exit(f"Could not open {port} ({e}). Close the Arduino Serial Monitor and retry.")
    lines = []
    with ser:
        print(f"Connected to {port}, waiting for the board to start...")
        deadline = time.time() + timeout
        while time.time() < deadline:                    # the board restarts when the port opens
            line = ser.readline().decode("ascii", "ignore").strip()
            if line.startswith("t_ms,"):
                break
        ser.write(b"dump\n")
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = ser.readline().decode("ascii", "ignore").strip()
            if line.startswith("#L,"):
                lines.append(line)
                if line.startswith("#L,END"):
                    return lines
    sys.exit("No complete log received. Is the WARDEN firmware uploaded?")


def check(lines):
    """Verify the chain of a dump (list of '#L,...' lines). Returns a dict, prints nothing:
    {"ok": bool, "records": [{"seq", "unix", "dt", "name", "value", "chain", "status", "alert"}],
     "issues": [str], "alerts": int}"""
    records, end = [], None
    for line in lines:
        parts = line.strip().split(",")
        if len(parts) < 2 or parts[1] == "seq":
            continue
        if parts[1] == "END":
            end = (int(parts[2]), int(parts[3], 16))
            continue
        seq, unix, dt, name, value, chain = parts[1:7]
        records.append({"seq": int(seq), "unix": int(unix), "dt": dt, "name": name,
                        "value": int(value), "chain": int(chain, 16)})

    ok, issues = True, []
    prev = 0xFFFF if records and records[0]["seq"] == 0 else None     # after wrap-around the first link is unknown
    for i, r in enumerate(records):
        raw = r["unix"].to_bytes(4, "little") + bytes([EVENT_CODES.get(r["name"], 0), r["value"]])
        if prev is None:
            r["status"] = "start"
        elif crc16(prev, raw) == r["chain"]:
            r["status"] = "ok"
        else:
            r["status"] = "BROKEN"
            ok = False
            issues.append(f"record {r['seq']} does not match the chain (edited or forged)")
        if i > 0 and r["seq"] != records[i - 1]["seq"] + 1:
            r["status"] += " (gap)"
            ok = False
            issues.append(f"records missing between {records[i - 1]['seq']} and {r['seq']}")
        r["alert"] = r["name"] in ALERTS
        prev = r["chain"]
    if records and end is not None:
        count, last_chain = end
        if count != records[-1]["seq"] + 1 or last_chain != records[-1]["chain"]:
            ok = False
            issues.append("the log header does not match the last record (records removed at the end?)")
    if not records and end is None:
        ok = False
        issues.append("no log received")
    return {"ok": ok, "records": records, "issues": issues, "alerts": sum(r["alert"] for r in records)}


def verify(lines):
    result = check(lines)
    records = result["records"]
    if not records:
        print("Log is empty.")
        return result["ok"]
    print(f"\n{'seq':>5}  {'date & time (crate clock)':<20}  {'event':<8}  {'value':>5}  check")
    for r in records:
        name, value = r["name"], r["value"]
        extra = f"{value / 10:.1f}g" if name in ("DROP", "SHOCK") else f"{value}/10 trees" if name == "TAMPER" else str(value)
        flag = "  <- ALERT" if r["alert"] else ""
        print(f"{r['seq']:>5}  {r['dt']:<20}  {name:<8}  {extra:>5}  {r['status']}{flag}")
    for issue in result["issues"]:
        print("  !", issue)
    print(f"\n{len(records)} records, {result['alerts']} alert(s).")
    print("INTEGRITY OK: the chain is unbroken." if result["ok"] else
          "INTEGRITY FAILED: the log was modified or damaged.")
    return result["ok"]


def main():
    ap = argparse.ArgumentParser(description="Read and verify the WARDEN tamper-evident log.")
    ap.add_argument("--file", help="verify a saved dump instead of reading the board")
    ap.add_argument("--save", help="also save the raw dump to this file")
    ap.add_argument("--port", help="serial port (auto-detected if omitted)")
    args = ap.parse_args()

    if args.file:
        with open(args.file) as f:
            lines = [l.strip() for l in f if l.startswith("#L,")]
    else:
        lines = read_from_board(args.port)
    if args.save:
        with open(args.save, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(f"Raw dump saved to {args.save}")
    sys.exit(0 if verify(lines) else 1)


if __name__ == "__main__":
    main()
