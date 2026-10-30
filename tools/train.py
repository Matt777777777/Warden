#!/usr/bin/env python3
"""
WARDEN - train the on-device motion classifier and export it for the Arduino.

    python tools/train.py

Pipeline
  1. Features: every session is replayed through firmware/warden/features.c, the SAME C
     code the crate runs (compiled here as a shared library), with two staggered
     accumulators: a 1 s window every 0.5 s, exactly like the firmware.
  2. Label cleaning: a session label says what happened *at some point* during 20 s.
     Calm windows are relabelled "still"; inside drop sessions, active windows with no
     free fall and no impact (lifting the crate) are ambiguous and left out.
     open / carry / flip are grouped as "handling": opening is judged by the light
     sensor + badge rule, orientation by the flip rule; the model only has to tell
     still, handling, drop and tamper apart.
  3. Honest evaluation: 5-fold cross-validation grouped by session (windows of one
     session are never split between training and test), at window level and at alert
     level, against the rule-based firmware v0.3 replayed on the same raw data.
  4. Export: a 10-tree random forest (hard majority vote) written to
     firmware/warden/model.h, plus a report in docs/ml/.
"""

import ctypes
import glob
import pickle
import platform
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import accuracy_score, confusion_matrix, f1_score
from sklearn.model_selection import GroupKFold

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "raw"
FW = ROOT / "firmware" / "warden"
BUILD = ROOT / "build"
DOCS = ROOT / "docs" / "ml"

CLASSES = ["still", "handling", "drop", "tamper"]
GROUP = {"still": "still", "open": "handling", "carry": "handling", "flip": "handling",
         "drop": "drop", "tamper": "tamper"}
N_TREES, MAX_DEPTH, MIN_LEAF = 10, 6, 3
TAMPER_VOTE_MIN, TAMPER_NEED, TAMPER_WINDOW = 0.8, 3, 4     # alert logic used on the crate
TAMPER_COOLDOWN = 20                                        # windows (10 s) between two alerts, as on the crate
LID_OPEN_LIGHT = 400                                        # same threshold as the firmware


# ---------------------------------------------------------------- features (shared C code)
def load_feature_lib():
    BUILD.mkdir(exist_ok=True)
    ext = "dylib" if platform.system() == "Darwin" else "so"
    lib_path = BUILD / f"libwardenfeatures.{ext}"
    src = FW / "features.c"
    if not lib_path.exists() or lib_path.stat().st_mtime < src.stat().st_mtime:
        subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", str(lib_path), str(src), "-lm"], check=True)
    return ctypes.CDLL(str(lib_path))


def feature_names():
    text = (FW / "features.c").read_text()
    block = text[text.index("WF_NAMES[WF_COUNT] = {") + 22: text.index("};", text.index("WF_NAMES"))]
    return [s.strip().strip('"') for s in block.replace("\n", " ").split(",") if s.strip()]


NAMES = feature_names()
NF = len(NAMES)


class WF(ctypes.Structure):
    """Mirror of the WardenFeatures struct in features.h."""
    _fields_ = ([("n", ctypes.c_ubyte)]
                + [(k, ctypes.c_float) for k in ("m_sum m_sq m_min m_max ax_sum ay_sum az_sum ax_sq ay_sq "
                                                 "az_sq g_sum g_max jerk_sum jerk_max m_prev").split()]
                + [("ff_count", ctypes.c_ubyte), ("hi_count", ctypes.c_ubyte), ("sharp_count", ctypes.c_ubyte)])


def session_windows(lib, df):
    """Replay one session like the firmware: window A starts at sample 0, window B at sample 50."""
    f = ctypes.c_float
    lib.wf_add.argtypes = [ctypes.POINTER(WF)] + [f] * 6
    lib.wf_compute.argtypes = [ctypes.POINTER(WF), ctypes.POINTER(f)]
    acc = [WF(), WF()]
    for a in acc:
        lib.wf_reset(ctypes.byref(a))
    out = (f * NF)()
    rows = []
    vals = df[["ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps"]].to_numpy(np.float32)
    for i, v in enumerate(vals):
        args = [float(x) for x in v]
        lib.wf_add(ctypes.byref(acc[0]), *args)
        if i >= 50:
            lib.wf_add(ctypes.byref(acc[1]), *args)
        for a in acc:
            if a.n >= 100:
                lib.wf_compute(ctypes.byref(a), out)
                rows.append(list(out))
                lib.wf_reset(ctypes.byref(a))
    return rows


def build_dataset(lib):
    rows = []
    files = sorted(f for f in DATA.glob("*_*.csv") if f.name.split("_")[0] in GROUP)
    for f in files:
        df = pd.read_csv(f)
        if (np.diff(df.t_ms.to_numpy()) > 50).any():
            print(f"  skipping {f.name}: gaps in the recording")
            continue
        label = f.name.split("_")[0]
        light = df.light.to_numpy()
        for k, fv in enumerate(session_windows(lib, df)):
            end = 99 + 50 * k                                   # last sample of window k
            lid_open = int(light[end - 99:end + 1].max() > LID_OPEN_LIGHT)
            rows.append([f.stem, label, k, lid_open] + fv)
    D = pd.DataFrame(rows, columns=["session", "label", "k", "lid_open"] + NAMES)
    quiet = (D.acc_std < 0.02) & (D.gyro_mean < 3)
    y = D.label.map(GROUP)
    y[quiet] = "still"
    impact = (D.freefall_frac >= 0.03) | (D.acc_max > 2.0)
    D["y"] = y
    D["train"] = ~((D.label == "drop") & ~quiet & ~impact)
    return D


# ---------------------------------------------------------------- model (hard-vote forest)
def make_forest(seed=0):
    return RandomForestClassifier(n_estimators=N_TREES, max_depth=MAX_DEPTH, min_samples_leaf=MIN_LEAF,
                                  class_weight="balanced", random_state=seed)


def hard_votes(forest, X):
    """Votes per class: each tree votes for the majority class of its leaf (what the crate computes)."""
    votes = np.zeros((len(X), len(CLASSES)))
    col = np.array([CLASSES.index(c) for c in forest.classes_])
    for est in forest.estimators_:
        leaf = est.predict(X).astype(int)    # each tree predicts an index into forest.classes_
        np.add.at(votes, (np.arange(len(X)), col[leaf]), 1)
    return votes / len(forest.estimators_)


def tamper_alerts(votes, lid_open=None):
    """Crate logic: 'tamper' voted by >= 80 % of the trees in 3 of the last 4 windows raises an alert.
    With lid_open given, windows where the lid is open are ignored (sensor fusion: the light + badge
    rule already judges openings, and a normal opening gesture looks a lot like forcing)."""
    hist, alerts, quiet = [], 0, 0
    for i, v in enumerate(votes):
        quiet = max(0, quiet - 1)
        if lid_open is not None and lid_open[i]:
            hist = (hist + ["lid"])[-TAMPER_WINDOW:]
            continue
        name = CLASSES[v.argmax()] if v.max() >= TAMPER_VOTE_MIN else "unknown"
        hist = (hist + [name])[-TAMPER_WINDOW:]
        if hist.count("tamper") >= TAMPER_NEED and quiet == 0:
            alerts += 1
            hist, quiet = [], TAMPER_COOLDOWN
    return alerts


def rule_alerts(df):
    """Firmware v0.3 rules replayed on raw samples. Returns (drop alerts, shock alerts)."""
    m = np.sqrt(df.ax_g ** 2 + df.ay_g ** 2 + df.az_g ** 2).to_numpy()
    t = df.t_ms.to_numpy()
    drops = shocks = ff = 0
    armed, peak, deadline, last_shock = False, 0.0, 0, -1e9
    for mi, ti in zip(m, t):
        if mi < 0.35:
            ff += 1
        else:
            if ff >= 6 and not armed:
                armed, deadline, peak = True, ti + 300, 0.0
            ff = 0
        if armed:
            peak = max(peak, mi)
            if ti >= deadline:
                armed = False
                drops += peak > 2.0
                last_shock = ti
        elif mi > 3.0 and ti - last_shock > 2000:
            last_shock = ti
            shocks += 1
    return drops, shocks


# ---------------------------------------------------------------- export to C
def float32_le(thr):
    """Largest float32 <= thr, so that (float)x <= T on the crate == x <= thr in scikit-learn."""
    t = np.float32(thr)
    if float(t) > thr:
        t = np.nextafter(t, np.float32(-np.inf))
    return t


def c_float(t):
    """C float literal that reads back to exactly the same float32 ('0f' is not valid C, '0.0f' is)."""
    txt = f"{float(t):.9g}"
    if not any(ch in txt for ch in ".e"):
        txt += ".0"
    return txt + "f"


def export_c(forest, path):
    feat, thr, left, right, roots = [], [], [], [], []
    for est in forest.estimators_:
        tr = est.tree_
        base = len(feat)
        roots.append(base)
        for node in range(tr.node_count):
            if tr.children_left[node] == -1:                     # leaf: class index in 'left'
                cls = forest.classes_[int(np.argmax(tr.value[node][0]))]
                feat.append(-1); thr.append(0.0); left.append(CLASSES.index(cls)); right.append(0)
            else:
                feat.append(int(tr.feature[node])); thr.append(float32_le(tr.threshold[node]))
                left.append(base + int(tr.children_left[node])); right.append(base + int(tr.children_right[node]))
    n = len(feat)
    lines = [
        "/* WARDEN motion model - GENERATED by tools/train.py, do not edit by hand.",
        f"   Random forest: {len(roots)} trees, max depth {MAX_DEPTH}, {n} nodes, {NF} features, hard majority vote.",
        f"   Classes: {', '.join(CLASSES)}. */",
        "#ifndef WARDEN_MODEL_H", "#define WARDEN_MODEL_H", "#include <stdint.h>",
        "#ifdef __AVR__", "#include <avr/pgmspace.h>", "#else",
        "#define PROGMEM", "#define pgm_read_byte(p) (*(const uint8_t *)(p))",
        "#define pgm_read_word(p) (*(const uint16_t *)(p))", "#define pgm_read_float(p) (*(const float *)(p))",
        "#endif", "",
        f"#define WM_TREES {len(roots)}", f"#define WM_CLASSES {len(CLASSES)}", f"#define WM_FEATURES {NF}",
        "enum { " + ", ".join(f"WM_{c.upper()}" for c in CLASSES) + " };",
        "static const char *const WM_CLASS_NAMES[WM_CLASSES] = {" + ", ".join(f'"{c}"' for c in CLASSES) + "};",
        "",
        "static const uint16_t WM_ROOT[WM_TREES] PROGMEM = {" + ", ".join(map(str, roots)) + "};",
        "static const int8_t WM_FEAT[] PROGMEM = {" + ", ".join(map(str, feat)) + "};",
        "static const float WM_THR[] PROGMEM = {" + ", ".join(c_float(t) for t in thr) + "};",
        "static const uint16_t WM_LEFT[] PROGMEM = {" + ", ".join(map(str, left)) + "};",
        "static const uint16_t WM_RIGHT[] PROGMEM = {" + ", ".join(map(str, right)) + "};",
        "",
        "/* Fills votes[WM_CLASSES] (number of trees per class) and returns the winning class. */",
        "static uint8_t wm_predict(const float *x, uint8_t *votes) {",
        "  uint8_t t, c, best = 0;",
        "  for (c = 0; c < WM_CLASSES; c++) votes[c] = 0;",
        "  for (t = 0; t < WM_TREES; t++) {",
        "    uint16_t node = pgm_read_word(&WM_ROOT[t]);",
        "    int8_t f;",
        "    while ((f = (int8_t)pgm_read_byte(&WM_FEAT[node])) >= 0)",
        "      node = (x[f] <= pgm_read_float(&WM_THR[node])) ? pgm_read_word(&WM_LEFT[node]) : pgm_read_word(&WM_RIGHT[node]);",
        "    votes[pgm_read_word(&WM_LEFT[node])]++;",
        "  }",
        "  for (c = 1; c < WM_CLASSES; c++) if (votes[c] > votes[best]) best = c;",
        "  return best;",
        "}",
        "#endif", "",
    ]
    path.write_text("\n".join(lines))
    return n


# ---------------------------------------------------------------- report helpers
def save_confusion_png(cm, path, title):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    norm = cm / cm.sum(axis=1, keepdims=True)
    fig, ax = plt.subplots(figsize=(5.2, 4.4))
    ax.imshow(norm, cmap="Blues", vmin=0, vmax=1)
    for i in range(len(CLASSES)):
        for j in range(len(CLASSES)):
            ax.text(j, i, f"{norm[i, j]:.0%}\n({cm[i, j]})", ha="center", va="center", fontsize=9,
                    color="white" if norm[i, j] > 0.6 else "#1d2733")
    ax.set_xticks(range(len(CLASSES)), CLASSES)
    ax.set_yticks(range(len(CLASSES)), CLASSES)
    ax.set_xlabel("predicted")
    ax.set_ylabel("true")
    ax.set_title(title, fontsize=11)
    fig.tight_layout()
    fig.savefig(path, dpi=150)


def main():
    if not DATA.exists():
        sys.exit("No data/raw folder: record sessions with tools/record.py first.")
    print("1/4  Computing features with firmware/warden/features.c ...")
    lib = load_feature_lib()
    D = build_dataset(lib)
    sessions = D.session.unique()
    print(f"     {len(sessions)} sessions, {len(D)} windows ({D.train.sum()} used for training)")
    print("     training labels:", D[D.train].y.value_counts().to_dict())

    print("2/4  Cross-validation by session (5 folds) ...")
    X = D[NAMES].to_numpy(np.float32)
    y = D.y.to_numpy()
    groups = D.session.to_numpy()
    votes = np.zeros((len(D), len(CLASSES)))
    for tr, te in GroupKFold(n_splits=5).split(X, y, groups):
        tr = tr[D.train.to_numpy()[tr]]
        forest = make_forest().fit(X[tr], y[tr])
        votes[te] = hard_votes(forest, X[te])
    pred = np.array([CLASSES[i] for i in votes.argmax(1)])
    mask = D.train.to_numpy()
    acc = accuracy_score(y[mask], pred[mask])
    f1 = f1_score(y[mask], pred[mask], average="macro")
    cm = confusion_matrix(y[mask], pred[mask], labels=CLASSES)
    print(f"     window level: accuracy {acc:.1%}, macro-F1 {f1:.3f}")

    print("3/4  Alert-level comparison with the rule-based firmware ...")
    rows = []
    for s in sessions:
        idx = np.where(D.session.to_numpy() == s)[0]
        cls = s.split("_")[0]
        rd, rs = rule_alerts(pd.read_csv(DATA / f"{s}.csv"))
        lid = D.lid_open.to_numpy()[idx]
        rows.append((cls, rd > 0, rs > 0, tamper_alerts(votes[idx]) > 0, tamper_alerts(votes[idx], lid) > 0))
    A = pd.DataFrame(rows, columns=["cls", "rule_drop", "rule_shock", "ml_tamper", "ml_tamper_fused"]).groupby("cls").mean()
    order = [c for c in ["still", "open", "carry", "flip", "drop", "tamper"] if c in A.index]
    A = A.loc[order]
    print((A * 100).round(0).astype(int).to_string())

    print("4/4  Training the final model on all clean windows and exporting it ...")
    final = make_forest().fit(X[mask], y[mask])
    n_nodes = export_c(final, FW / "model.h")
    with open(BUILD / "forest.pkl", "wb") as fh:            # reused by tools/check_parity.py
        pickle.dump(final, fh)
    imp = sorted(zip(final.feature_importances_, NAMES), reverse=True)

    DOCS.mkdir(parents=True, exist_ok=True)
    save_confusion_png(cm, DOCS / "confusion_matrix.png", f"Leave-session-out, macro-F1 {f1:.2f}")
    pct = lambda v: f"{v * 100:.0f} %"
    report = [
        "# WARDEN motion model: training report", "",
        "Generated by `tools/train.py`.", "",
        "## Data", "",
        f"{len(sessions)} labelled sessions of 20 s recorded in the final crate, {len(D)} windows of 1 s (one every 0.5 s), "
        f"{int(mask.sum())} used for training after label cleaning.", "",
        "| Class | Training windows |", "|---|---|",
        *[f"| {c} | {int((y[mask] == c).sum())} |" for c in CLASSES], "",
        "## Model", "",
        f"Random forest, {N_TREES} trees, max depth {MAX_DEPTH}, {n_nodes} nodes, {NF} features computed on the fly "
        f"(`firmware/warden/features.c`), hard majority vote. Exported to `firmware/warden/model.h` "
        f"(about {n_nodes * 9 / 1024:.1f} KB of flash, no RAM besides the 17 feature values).", "",
        "## Window-level accuracy (5-fold cross-validation grouped by session)", "",
        f"Accuracy {acc:.1%}, macro-F1 {f1:.3f}. Windows of a given session are never split between training and test.", "",
        "![Confusion matrix](confusion_matrix.png)", "",
        "## Alerts: machine learning vs the rule-based firmware", "",
        "Share of sessions raising at least one alert (rules replayed on the same raw data; ML predictions are "
        "cross-validated, i.e. on sessions the model never saw).", "",
        "| Session type | Rule DROP | Rule SHOCK | ML TAMPER | ML TAMPER + lid sensor |", "|---|---|---|---|---|",
        *[f"| {c} | {pct(r.rule_drop)} | {pct(r.rule_shock)} | {pct(r.ml_tamper)} | {pct(r.ml_tamper_fused)} |"
          for c, r in A.iterrows()], "",
        f"ML tamper alert = at least {TAMPER_NEED} of the last {TAMPER_WINDOW} windows classified *tamper* by at least "
        f"{TAMPER_VOTE_MIN:.0%} of the trees. In the last column, windows where the light sensor sees the lid open are "
        "ignored: openings are judged by the light + badge rule, and a normal opening gesture resembles forcing. Drops stay with the physical rule (free fall then impact), which is already "
        "near perfect; flips stay with the orientation rule. The model takes over what the rules cannot do: telling forced "
        "entry from normal handling.", "",
        "## Most useful features", "",
        "| Feature | Importance |", "|---|---|",
        *[f"| {n} | {v:.3f} |" for v, n in imp[:8]], "",
        "## Known limits", "",
        "No road-transport data was collected: in a vehicle the model will see vibrations it has never learnt. "
        "One crate, one operator and one day of recordings: performance on other crates is not established.", "",
    ]
    (DOCS / "report.md").write_text("\n".join(report))
    print(f"     model.h written ({n_nodes} nodes), report in docs/ml/report.md")


if __name__ == "__main__":
    main()
