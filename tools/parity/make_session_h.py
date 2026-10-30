#!/usr/bin/env python3
"""Turn the first N samples of a session CSV into session.h (exact integers, stored in flash) for
avr_bench.c, plus trunc.csv (the same samples) for the laptop harness. See README.md here."""
import sys


def to_int(text, decimals):
    """'-0.012' with 3 decimals -> -12, without going through a float (no rounding)."""
    neg = text.startswith("-")
    whole, _, frac = text.lstrip("-").partition(".")
    assert len(frac) <= decimals, text
    value = int(whole or "0") * 10 ** decimals + int((frac + "0" * decimals)[:decimals])
    return -value if neg else value


def main():
    path, n = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 800
    lines = open(path).readlines()[: n + 1]
    rows = [l.strip().split(",") for l in lines[1:]]
    acc = [[to_int(r[j], 3) for j in (1, 2, 3)] for r in rows]      # milli-g
    gyr = [[to_int(r[j], 2) for j in (4, 5, 6)] for r in rows]      # centi-deg/s (can exceed int16)
    with open("session.h", "w") as f:
        f.write("#include <avr/pgmspace.h>\n#define SESSION_N %d\n" % len(rows))
        f.write("static const int16_t ACC[][3] PROGMEM = {" + ",".join("{%d,%d,%d}" % tuple(a) for a in acc) + "};\n")
        f.write("static const int32_t GYR[][3] PROGMEM = {" + ",".join("{%dL,%dL,%dL}" % tuple(g) for g in gyr) + "};\n")
    with open("trunc.csv", "w") as f:
        f.writelines(lines)


if __name__ == "__main__":
    main()
