#!/usr/bin/env python3
"""Download and segment the CWRU bearing dataset into 1024-sample frames (WBS P3.1).

Classes (12 kHz drive-end data, docs/Project_Scope_Statement.md §3):

    0 normal  97.mat    1 ball  118.mat    2 inner  105.mat    3 outer  130.mat

Steps:
1. With --download, fetch the four .mat files into data/raw/cwru/.
2. Read each file's drive-end accelerometer channel (X<NNN>_DE_time).
3. Split each recording in time into train/val/test segments (70/15/15 by
   default) *before* windowing. Overlapping windows then never span two
   splits, which keeps the test set free of samples seen in training.
4. Cut each segment into 1024-sample windows. The hop is --train-stride for
   the training segment (overlap augments the data) and 1024 elsewhere.

Outputs in data/processed/:
    cwru_windows.npz   X_{train,val,test} float32 (N, 1024), y_{train,val,test} int64,
                       classes, sample_rate, frame_len
    frames.f32         raw float32 test frames for `pdm_bench --frames`,
                       in blocks of --replay-block frames per class
                       (normal, ball, inner, outer, normal, ...)
    frames_labels.csv  frame index -> class of frames.f32
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

import numpy as np

FRAME_LEN = 1024
SAMPLE_RATE = 12000
CLASSES = [("normal", 97), ("ball", 118), ("inner", 105), ("outer", 130)]
URL = "https://engineering.case.edu/sites/default/files/{num}.mat"


def download(raw_dir: Path) -> None:
    raw_dir.mkdir(parents=True, exist_ok=True)
    for _, num in CLASSES:
        dest = raw_dir / f"{num}.mat"
        if dest.exists():
            print(f"have {dest}")
            continue
        print(f"downloading {URL.format(num=num)}")
        tmp = dest.with_suffix(".part")
        urllib.request.urlretrieve(URL.format(num=num), tmp)
        tmp.rename(dest)


def load_de_signal(path: Path, num: int) -> np.ndarray:
    """Return the drive-end vibration channel of one CWRU .mat file."""
    from scipy.io import loadmat

    mat = loadmat(path)
    preferred = f"X{num:03d}_DE_time"
    if preferred in mat:
        key = preferred
    else:
        # Some files name the channel after a different record number.
        candidates = sorted(k for k in mat if k.endswith("_DE_time"))
        if not candidates:
            raise ValueError(f"{path}: no *_DE_time variable (found {sorted(k for k in mat if not k.startswith('__'))})")
        key = candidates[0]
        print(f"{path.name}: {preferred} missing, using {key}", file=sys.stderr)
    return np.asarray(mat[key], dtype=np.float32).ravel()


def windows(signal: np.ndarray, stride: int) -> np.ndarray:
    if len(signal) < FRAME_LEN:
        return np.empty((0, FRAME_LEN), dtype=np.float32)
    starts = range(0, len(signal) - FRAME_LEN + 1, stride)
    return np.stack([signal[s:s + FRAME_LEN] for s in starts]).astype(np.float32)


def split_bounds(n: int, val: float, test: float) -> tuple[int, int]:
    train_end = int(round(n * (1.0 - val - test)))
    val_end = int(round(n * (1.0 - test)))
    return train_end, val_end


def replay_order(y: np.ndarray, block: int) -> np.ndarray:
    """Index order that cycles through classes in blocks of `block` frames."""
    per_class = [list(np.flatnonzero(y == c)) for c in range(len(CLASSES))]
    order: list[int] = []
    while any(per_class):
        for idx in per_class:
            order.extend(idx[:block])
            del idx[:block]
    return np.asarray(order, dtype=np.int64)


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=here / "raw" / "cwru")
    ap.add_argument("--out", type=Path, default=here / "processed")
    ap.add_argument("--download", action="store_true", help="fetch missing .mat files first")
    ap.add_argument("--train-stride", type=int, default=512, help="window hop in the training split")
    ap.add_argument("--val", type=float, default=0.15, help="fraction of each recording for validation")
    ap.add_argument("--test", type=float, default=0.15, help="fraction of each recording for test")
    ap.add_argument("--replay-block", type=int, default=16, help="consecutive frames per class in frames.f32")
    args = ap.parse_args()

    if not 0 < args.train_stride <= FRAME_LEN:
        ap.error(f"--train-stride must be in 1..{FRAME_LEN}")
    if args.val < 0 or args.test < 0 or args.val + args.test >= 1:
        ap.error("--val and --test must be non-negative and sum to less than 1")

    if args.download:
        download(args.raw)

    splits: dict[str, tuple[list[np.ndarray], list[np.ndarray]]] = {s: ([], []) for s in ("train", "val", "test")}
    for label, (name, num) in enumerate(CLASSES):
        path = args.raw / f"{num}.mat"
        if not path.exists():
            print(f"missing {path}; run with --download or copy the CWRU files there", file=sys.stderr)
            return 1
        sig = load_de_signal(path, num)
        a, b = split_bounds(len(sig), args.val, args.test)
        parts = {
            "train": windows(sig[:a], args.train_stride),
            "val": windows(sig[a:b], FRAME_LEN),
            "test": windows(sig[b:], FRAME_LEN),
        }
        for split, w in parts.items():
            splits[split][0].append(w)
            splits[split][1].append(np.full(len(w), label, dtype=np.int64))
        print(f"{name:6s} {path.name}: {len(sig)} samples ({len(sig) / SAMPLE_RATE:.1f} s) -> "
              + ", ".join(f"{s} {len(w)}" for s, w in parts.items()))

    arrays = {}
    for split, (xs, ys) in splits.items():
        arrays[f"X_{split}"] = np.concatenate(xs)
        arrays[f"y_{split}"] = np.concatenate(ys)

    args.out.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.out / "cwru_windows.npz", **arrays,
                        classes=np.array([c for c, _ in CLASSES]),
                        sample_rate=SAMPLE_RATE, frame_len=FRAME_LEN)

    order = replay_order(arrays["y_test"], args.replay_block)
    arrays["X_test"][order].astype("<f4").tofile(args.out / "frames.f32")
    with open(args.out / "frames_labels.csv", "w") as f:
        f.write("frame,class_id,class\n")
        for i, idx in enumerate(order):
            c = int(arrays["y_test"][idx])
            f.write(f"{i},{c},{CLASSES[c][0]}\n")

    print(f"wrote {args.out / 'cwru_windows.npz'} "
          f"(train {len(arrays['y_train'])}, val {len(arrays['y_val'])}, test {len(arrays['y_test'])})")
    print(f"wrote {args.out / 'frames.f32'} ({len(order)} frames) and frames_labels.csv")
    return 0


if __name__ == "__main__":
    sys.exit(main())
