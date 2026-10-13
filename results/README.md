# Results and analysis

- `raw/`: per-cycle CSV logs from `benchmarks/run_matrix.sh`, named `<config>_<level>[_rN].csv`.
- `analyze.py`: computes the KPIs and plots (WBS P6.2 and P6.3).
- `summary.csv` and `summary.md`: the KPI table, generated.
- `plots/`: the figures, generated.

```sh
pip install -r results/requirements.txt
python3 results/analyze.py                       # reads results/raw, writes results/
python3 results/analyze.py --tolerance-ms 0.5 --no-plots
```

## KPIs

KPIs are computed per (config, stress level). Repeats (`_r1`, `_r2`, ...) are pooled
into one sample.

| Column | Meaning |
| --- | --- |
| `mean_ms`, `sigma_ms` | mean and standard deviation of the period T. σ_J = σ_T because J = T − 10 ms. |
| `p99_ms`, `p999_ms`, `wcl_ms` | 99th and 99.9th percentiles, and the worst case (max T) |
| `dmr_strict_pct` | share of periods with T > 10.0 ms, the strict rule from the scope statement |
| `dmr_tol_pct` | share of periods with T > 10.0 ms + `--tolerance-ms` (default 10.5 ms) |
| `severe` | periods with T > 50 ms |
| `nivcsw`, `minflt` | increase over the run in involuntary context switches and minor faults, when the log has these columns |

Periods are computed from `wakeup_ns` when that column is present. Otherwise the
logged `period_ns` values are used, skipping the leading 0.

The strict DMR is close to 50 % for any timer with symmetric jitter, because half of
all periods land slightly above 10 ms. For that reason both DMR columns are reported.

## Plots

- `cdf_<level>.png`: the complementary CDF of T on a log scale, one curve per config.
  It shows the tail behaviour that WCL and the P99.9 summarize.
- `boxplot.png`: T per config with whiskers from P0.1 to P99.9, one panel per stress level.
- `jitter_hist.png`: histograms of J = T − T_nom.
- `timeline_<config>_<level>.png`: T against cycle index, to show when spikes occur.
