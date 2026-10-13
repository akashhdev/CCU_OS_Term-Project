#!/usr/bin/env python3
"""Compute benchmark KPIs and plots from per-cycle CSV logs (WBS P6.2, P6.3).

Input files are named ``<config>_<level>[_r<N>].csv`` (for example ``A_idle.csv``
or ``C_100_r2.csv``), as written by ``benchmarks/run_matrix.sh``. Each file
needs a ``wakeup_ns`` column or a ``period_ns`` column. When both are present,
``wakeup_ns`` is used. Repeats of the same configuration and level are pooled.

The KPIs follow docs/Project_Scope_Statement.md §4.3.2 and §8:

* mean and standard deviation of the cycle period T, plus sigma_J of the
  jitter J = T - T_nom (equal to sigma_T)
* worst-case cycle latency WCL = max T, plus the 99th and 99.9th percentiles
* DMR under the strict rule T > T_nom, and under a tolerance rule
  T > T_nom + tol (default tol = 0.5 ms, matching the WCL < 10.5 ms target)
* severe starvation count, T > 50 ms

Outputs, by default under results/:
    summary.csv, summary.md
    plots/cdf_<level>.png, plots/boxplot.png, plots/jitter_hist.png,
    plots/timeline_<config>_<level>.png

Usage:
    python3 results/analyze.py [--raw results/raw] [--out results]
                               [--nominal-ms 10] [--tolerance-ms 0.5]
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
import pandas as pd

FILENAME_RE = re.compile(r"^(?P<config>[A-Za-z0-9]+)_(?P<level>[A-Za-z0-9]+)(?:_r(?P<rep>\d+))?\.csv$")
CONFIG_LABELS = {"A": "A: Linux CFS", "B": "B: Linux SCHED_FIFO", "C": "C: Dual-OS (Zephyr)"}
LEVEL_ORDER = ["idle", "0", "50", "100"]
SEVERE_MS = 50.0


def level_key(level: str) -> tuple[int, str]:
    return (LEVEL_ORDER.index(level) if level in LEVEL_ORDER else len(LEVEL_ORDER), level)


def level_label(level: str) -> str:
    return "idle" if level in ("idle", "0") else f"{level}% stress"


def load_periods_ms(path: Path) -> np.ndarray:
    """Return the cycle periods of one log, in milliseconds."""
    df = pd.read_csv(path)
    if "wakeup_ns" in df.columns:
        periods = np.diff(df["wakeup_ns"].to_numpy(dtype=np.int64))
    elif "period_ns" in df.columns:
        periods = df["period_ns"].to_numpy(dtype=np.int64)
        # Loggers emit period 0 for the first cycle, which has no predecessor.
        periods = periods[periods > 0]
    else:
        raise ValueError(f"{path}: needs a wakeup_ns or period_ns column")
    return periods / 1e6


def load_counters(path: Path) -> dict[str, int]:
    """Return the increase over the run of cumulative OS counters, if logged."""
    df = pd.read_csv(path)
    return {col: int(df[col].iloc[-1] - df[col].iloc[0]) for col in ("nivcsw", "minflt") if col in df.columns}


def kpis(periods: np.ndarray, nominal_ms: float, tolerance_ms: float) -> dict[str, float]:
    n = len(periods)
    jitter = periods - nominal_ms
    strict = int(np.count_nonzero(periods > nominal_ms))
    tol = int(np.count_nonzero(periods > nominal_ms + tolerance_ms))
    return {
        "periods": n,
        "mean_ms": float(periods.mean()),
        "sigma_ms": float(periods.std(ddof=1)) if n > 1 else 0.0,
        "min_ms": float(periods.min()),
        "p50_ms": float(np.percentile(periods, 50)),
        "p99_ms": float(np.percentile(periods, 99)),
        "p999_ms": float(np.percentile(periods, 99.9)),
        "wcl_ms": float(periods.max()),
        "max_abs_jitter_ms": float(np.abs(jitter).max()),
        "misses_strict": strict,
        "dmr_strict_pct": 100.0 * strict / n,
        "misses_tol": tol,
        "dmr_tol_pct": 100.0 * tol / n,
        "severe": int(np.count_nonzero(periods > SEVERE_MS)),
    }


def collect(raw_dir: Path) -> tuple[dict[tuple[str, str], np.ndarray], dict[tuple[str, str], dict[str, int]]]:
    periods: dict[tuple[str, str], list[np.ndarray]] = defaultdict(list)
    counters: dict[tuple[str, str], dict[str, int]] = defaultdict(lambda: defaultdict(int))
    for path in sorted(raw_dir.glob("*.csv")):
        m = FILENAME_RE.match(path.name)
        if not m:
            print(f"skipping {path.name}: name is not <config>_<level>[_rN].csv", file=sys.stderr)
            continue
        key = (m["config"], m["level"])
        p = load_periods_ms(path)
        if len(p) == 0:
            print(f"skipping {path.name}: no periods", file=sys.stderr)
            continue
        periods[key].append(p)
        for col, delta in load_counters(path).items():
            counters[key][col] += delta
    return {k: np.concatenate(v) for k, v in periods.items()}, counters


def write_summary(rows: list[dict], out_dir: Path, nominal_ms: float, tolerance_ms: float) -> pd.DataFrame:
    df = pd.DataFrame(rows)
    for col in ("nivcsw", "minflt"):
        if col in df.columns:
            df[col] = df[col].astype("Int64")  # stays integer when some runs lack counters
    df.to_csv(out_dir / "summary.csv", index=False, float_format="%.6f")

    lines = [
        f"| Config | Stress | Periods | Mean (ms) | σ_J (ms) | P99.9 (ms) | WCL (ms) "
        f"| DMR strict >{nominal_ms:g} ms | DMR >{nominal_ms + tolerance_ms:g} ms | Severe >{SEVERE_MS:g} ms |",
        "| :-- | :-- | --: | --: | --: | --: | --: | --: | --: | --: |",
    ]
    for r in rows:
        lines.append(
            f"| {r['config']} | {level_label(r['level'])} | {r['periods']} | {r['mean_ms']:.4f} "
            f"| {r['sigma_ms']:.4f} | {r['p999_ms']:.3f} | {r['wcl_ms']:.3f} "
            f"| {r['dmr_strict_pct']:.2f}% | {r['dmr_tol_pct']:.2f}% | {r['severe']} |"
        )
    (out_dir / "summary.md").write_text("\n".join(lines) + "\n")
    return df


def plot_all(data: dict[tuple[str, str], np.ndarray], plot_dir: Path, nominal_ms: float, tolerance_ms: float) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plot_dir.mkdir(parents=True, exist_ok=True)
    configs = sorted({c for c, _ in data})
    levels = sorted({lv for _, lv in data}, key=level_key)
    colors = {c: f"C{i}" for i, c in enumerate(configs)}

    def mark_deadlines(ax, axis="x"):
        line = ax.axvline if axis == "x" else ax.axhline
        line(nominal_ms, color="black", lw=0.8, ls="--", label=f"T_nom = {nominal_ms:g} ms")
        line(nominal_ms + tolerance_ms, color="red", lw=0.8, ls=":", label=f"{nominal_ms + tolerance_ms:g} ms")

    # CDF per stress level: one curve per configuration, log-scaled tail.
    for level in levels:
        fig, ax = plt.subplots(figsize=(6.4, 4.2))
        for config in configs:
            p = data.get((config, level))
            if p is None:
                continue
            x = np.sort(p)
            ccdf = 1.0 - np.arange(len(x)) / len(x)
            ax.step(x, ccdf, where="post", color=colors[config], label=CONFIG_LABELS.get(config, config))
        mark_deadlines(ax)
        ax.set_yscale("log")
        ax.set_xlabel("Cycle period T (ms)")
        ax.set_ylabel("P(period ≥ T)")
        ax.set_title(f"Cycle period tail distribution, {level_label(level)}")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(plot_dir / f"cdf_{level}.png", dpi=150)
        plt.close(fig)

    # Box plots: one panel per stress level, configs side by side.
    fig, axes = plt.subplots(1, len(levels), figsize=(3.6 * len(levels), 4.2), sharey=True, squeeze=False)
    for ax, level in zip(axes[0], levels):
        present = [c for c in configs if (c, level) in data]
        bp = ax.boxplot([data[(c, level)] for c in present], whis=(0.1, 99.9), showfliers=True,
                        flierprops={"markersize": 2, "alpha": 0.4}, patch_artist=True)
        for patch, c in zip(bp["boxes"], present):
            patch.set_facecolor(colors[c])
            patch.set_alpha(0.5)
        ax.set_xticks(range(1, len(present) + 1), present)
        mark_deadlines(ax, axis="y")
        ax.set_title(level_label(level))
        ax.grid(True, axis="y", alpha=0.3)
    axes[0][0].set_ylabel("Cycle period T (ms)")
    axes[0][-1].legend(fontsize=7, loc="upper right")
    fig.suptitle("Cycle period by configuration (whiskers: P0.1 to P99.9)")
    fig.tight_layout()
    fig.savefig(plot_dir / "boxplot.png", dpi=150)
    plt.close(fig)

    # Jitter histograms: one row per stress level.
    fig, axes = plt.subplots(len(levels), 1, figsize=(6.4, 2.4 * len(levels)), sharex=True, squeeze=False)
    for ax, level in zip(axes[:, 0], levels):
        for config in configs:
            p = data.get((config, level))
            if p is not None:
                ax.hist(p - nominal_ms, bins=200, histtype="step", color=colors[config],
                        label=CONFIG_LABELS.get(config, config))
        ax.set_yscale("log")
        ax.set_ylabel("cycles")
        ax.set_title(level_label(level), fontsize=9)
        ax.grid(True, alpha=0.3)
    axes[-1, 0].set_xlabel("Jitter J = T - T_nom (ms)")
    axes[0, 0].legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(plot_dir / "jitter_hist.png", dpi=150)
    plt.close(fig)

    # Per-run timelines, to show when the spikes happen.
    for (config, level), p in data.items():
        fig, ax = plt.subplots(figsize=(8, 2.6))
        ax.plot(np.arange(1, len(p) + 1), p, lw=0.5, color=colors[config])
        mark_deadlines(ax, axis="y")
        ax.set_xlabel("cycle")
        ax.set_ylabel("T (ms)")
        ax.set_title(f"{CONFIG_LABELS.get(config, config)}, {level_label(level)}", fontsize=9)
        ax.grid(True, alpha=0.3)
        fig.tight_layout()
        fig.savefig(plot_dir / f"timeline_{config}_{level}.png", dpi=150)
        plt.close(fig)


def main() -> int:
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--raw", type=Path, default=here / "raw", help="directory holding the CSV logs")
    ap.add_argument("--out", type=Path, default=here, help="where summary.* and plots/ are written")
    ap.add_argument("--nominal-ms", type=float, default=10.0, help="nominal control period")
    ap.add_argument("--tolerance-ms", type=float, default=0.5, help="tolerance for the second DMR column")
    ap.add_argument("--no-plots", action="store_true", help="only compute the summary")
    args = ap.parse_args()

    data, counters = collect(args.raw)
    if not data:
        print(f"no usable CSV logs in {args.raw}", file=sys.stderr)
        return 1

    rows = []
    for config, level in sorted(data, key=lambda k: (k[0], level_key(k[1]))):
        row = {"config": config, "level": level}
        row.update(kpis(data[(config, level)], args.nominal_ms, args.tolerance_ms))
        row.update(counters.get((config, level), {}))
        rows.append(row)

    args.out.mkdir(parents=True, exist_ok=True)
    write_summary(rows, args.out, args.nominal_ms, args.tolerance_ms)
    print((args.out / "summary.md").read_text(), end="")
    if not args.no_plots:
        plot_all(data, args.out / "plots", args.nominal_ms, args.tolerance_ms)
        print(f"plots written to {args.out / 'plots'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
