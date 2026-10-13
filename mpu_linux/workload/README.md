# pdm_bench: Linux benchmark for Configurations A and B

A dual-threaded C benchmark that implements the workload-normalized design in
`docs/Project_Scope_Statement.md` §4.3.1 (WBS task P2.4).

| Thread | Period | Work |
| --- | --- | --- |
| Reflex (timed) | 10 ms, `clock_nanosleep(TIMER_ABSTIME)` | samples the latest setpoint, records its wakeup timestamp |
| Cognitive | ~85 ms | Hann window, 1024-point FFT, fault classification, derating LUT (100/85/70/60 %) |

The two threads share the setpoint through one atomic variable ("latest value
wins"), so the reflex tick never waits on analytics.

The fault classifier is a rule-based placeholder. It looks for spectral energy at the
SKF 6205 defect frequencies (ball 141 Hz, inner race 162 Hz, outer race 107 Hz).
The TFLite 1D-CNN replaces it in P3.5.

## Build

```sh
make            # needs gcc and glibc (pthreads, libm)
```

## Run

```sh
# Config A: CFS
./pdm_bench -s cfs  -o ../../results/raw/A_idle.csv

# Config B: SCHED_FIFO priority 90 for the reflex thread (needs root or CAP_SYS_NICE)
sudo ./pdm_bench -s fifo -r 90 -o ../../results/raw/B_idle.csv

# Replay CWRU frames (float32, 1024 samples per frame) instead of synthetic ones
./pdm_bench -f ../../data/processed/frames.f32 -o out.csv
```

Run `./pdm_bench --help` to list every option, such as `--mlock`, `--cpu`, and `--no-analytics`.

A one-line summary is printed to stderr at the end of each run:

```
policy=cfs periods=9999 mean=10.0001 ms sigma=0.0954 ms wcl=10.9280 ms misses=... dmr=...% severe=0 analytics_runs=1177
```

## CSV format

`cycle,wakeup_ns,period_ns,setpoint_pct,nivcsw,minflt`

- `wakeup_ns`: `CLOCK_MONOTONIC` wakeup time of the cycle.
- `period_ns`: wakeup-to-wakeup period. It is 0 on cycle 0.
- `setpoint_pct`: the setpoint the reflex thread saw in this cycle.
- `nivcsw`, `minflt`: the reflex thread's cumulative involuntary context switches and
  minor page faults, from `getrusage(RUSAGE_THREAD)`.

Samples are kept in a buffer that is allocated and touched before the run. The CSV
is written only after the last cycle, so the timed loop does no I/O. If a cycle
overruns by more than one period, the slots it missed are skipped rather than
fired back-to-back. That overrun shows up as a single long period.
