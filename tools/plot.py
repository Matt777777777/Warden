#!/usr/bin/env python3
"""
WARDEN - look at a recorded session.

Usage
    python tools/plot.py data/raw/drop_20261021-142530.csv
    python tools/plot.py data/raw/drop_20261021-142530.csv --save   # also writes a PNG next to it
    python tools/plot.py --latest                                    # newest file in data/raw
"""

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser(description="Plot a WARDEN session CSV.")
    ap.add_argument("csv", nargs="?", help="path to a session CSV")
    ap.add_argument("--latest", action="store_true", help="plot the most recent session")
    ap.add_argument("--save", action="store_true", help="save a PNG instead of only showing it")
    args = ap.parse_args()

    if args.latest:
        files = sorted((ROOT / "data" / "raw").glob("*_*.csv"), key=lambda p: p.stat().st_mtime)
        files = [f for f in files if f.name != "sessions_log.csv"]
        if not files:
            ap.error("no session files in data/raw yet")
        path = files[-1]
    elif args.csv:
        path = Path(args.csv)
    else:
        ap.error("give a CSV path or --latest")

    df = pd.read_csv(path)
    t = (df["t_ms"] - df["t_ms"].iloc[0]) / 1000.0
    acc_mag = np.sqrt(df["ax_g"] ** 2 + df["ay_g"] ** 2 + df["az_g"] ** 2)
    label = df["label"].iloc[0] if "label" in df else path.stem

    fig, axes = plt.subplots(4, 1, figsize=(12, 9), sharex=True)
    fig.suptitle(f"{path.name}  ({label}, {len(df)} samples)")

    for col in ("ax_g", "ay_g", "az_g"):
        axes[0].plot(t, df[col], label=col, lw=0.9)
    axes[0].set_ylabel("acceleration (g)")
    axes[0].legend(loc="upper right")

    axes[1].plot(t, acc_mag, color="black", lw=0.9)
    axes[1].axhline(1.0, color="grey", ls="--", lw=0.8)
    axes[1].set_ylabel("|a| (g)\n1 = at rest")

    for col in ("gx_dps", "gy_dps", "gz_dps"):
        axes[2].plot(t, df[col], label=col, lw=0.9)
    axes[2].set_ylabel("rotation (deg/s)")
    axes[2].legend(loc="upper right")

    axes[3].plot(t, df["light"], color="orange", lw=1.2)
    axes[3].set_ylabel("light (0-1023)")
    axes[3].set_xlabel("time (s)")

    for ax in axes:
        ax.grid(alpha=0.3)
    fig.tight_layout()

    if args.save:
        out = path.with_suffix(".png")
        fig.savefig(out, dpi=120)
        print(f"saved {out}")
    plt.show()


if __name__ == "__main__":
    main()
