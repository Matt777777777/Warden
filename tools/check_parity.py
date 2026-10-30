#!/usr/bin/env python3
"""
WARDEN - check that the crate will vote exactly like the model trained in Python.

    python tools/train.py          # first: trains, writes model.h and build/forest.pkl
    python tools/check_parity.py

The firmware's own C files (features.c + model.h) are compiled on the laptop into a small
harness, every recorded session is replayed through it, and the votes of every window are
compared with scikit-learn's. Like checking a translated contract clause by clause:
any disagreement means the export changed the meaning somewhere.
"""

import pickle
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent))
import train as T  # noqa: E402

HARNESS_SRC = T.ROOT / "tools" / "parity" / "harness.c"
HARNESS_BIN = T.BUILD / "parity_harness"


def main():
    forest_path = T.BUILD / "forest.pkl"
    if not forest_path.exists():
        sys.exit("build/forest.pkl not found: run python tools/train.py first.")
    with open(forest_path, "rb") as fh:
        forest = pickle.load(fh)

    print("1/3  Compiling features.c + model.h into a laptop harness ...")
    subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-o", str(HARNESS_BIN), str(HARNESS_SRC),
                    str(T.FW / "features.c"), "-lm"], check=True)

    print("2/3  Python side: features through ctypes, votes from scikit-learn ...")
    lib = T.load_feature_lib()
    D = T.build_dataset(lib)
    py_votes = np.rint(T.hard_votes(forest, D[T.NAMES].to_numpy(np.float32)) * T.N_TREES).astype(int)

    print("3/3  C side: replaying every session through the harness ...")
    files = [T.DATA / f"{s}.csv" for s in D.session.unique()]
    out = subprocess.run([str(HARNESS_BIN)] + [str(f) for f in files], check=True,
                         capture_output=True, text=True).stdout.split("\n")
    rows = [r.split() for r in out if r]
    C = pd.DataFrame([(Path(r[0]).stem, int(r[1]), *map(int, r[2:])) for r in rows],
                     columns=["session", "k"] + T.CLASSES)

    if len(C) != len(D):
        sys.exit(f"MISMATCH: {len(C)} windows in C vs {len(D)} in Python")
    c_votes = C[T.CLASSES].to_numpy()
    same_votes = (c_votes == py_votes).all(axis=1)
    same_class = c_votes.argmax(axis=1) == py_votes.argmax(axis=1)
    print(f"\n     {len(D)} windows from {len(files)} sessions")
    print(f"     identical votes (all 10 trees): {same_votes.mean():.2%}")
    print(f"     identical predicted class:      {same_class.mean():.2%}")
    if not same_votes.all():
        bad = np.where(~same_votes)[0][:10]
        for i in bad:
            print(f"     differs: {D.session.iat[i]} window {D.k.iat[i]}  C={c_votes[i]}  Python={py_votes[i]}")
        sys.exit(1)
    print("\nPARITY OK: the crate will vote exactly like the trained model.")


if __name__ == "__main__":
    main()
