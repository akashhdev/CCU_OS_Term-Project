# Benchmark harness

`run_matrix.sh` runs the matrix of 3 configurations × 3 stress levels from the
scope statement (WBS P2.8 and P6.1). It writes one CSV per run, plus a `.meta`
file, to `results/raw/`.

## Prerequisites (on the board)

```sh
sudo apt install stress-ng build-essential
make -C mpu_linux/workload        # builds pdm_bench
```

## Usage

```sh
benchmarks/run_matrix.sh                     # Configs A and B, idle/50/100, 10,000 cycles
benchmarks/run_matrix.sh -c "A B C" -r 3     # all configs, 3 repeats each (*_r1.csv ...)
benchmarks/run_matrix.sh -c B -l 100 -n 2000 # one quick cell
benchmarks/run_matrix.sh -d                  # dry run: print commands only
SERIAL_DEV=/dev/ttyHS1 benchmarks/run_matrix.sh -c C   # Config C
```

| Variable | Purpose | Default |
| --- | --- | --- |
| `PDM_BENCH` | path to the benchmark binary | `mpu_linux/workload/pdm_bench` |
| `BENCH_ARGS` | extra `pdm_bench` flags, e.g. `"--mlock --cpu 3"` | none |
| `SERIAL_DEV`, `SERIAL_BAUD` | Zephyr log port for Config C | unset, `115200` |
| `SETTLE_S` | seconds to let stress-ng ramp up before measuring | `5` |
| `STRESS_CPUS`, `STRESS_VM_BYTES_50`, `STRESS_VM_BYTES_100` | stress level tuning | `nproc`, `128M`, `256M` |

## Stress levels

The levels are defined in `mpu_linux/stress/stress_levels.sh`:

| Level | stress-ng arguments |
| --- | --- |
| `idle` | (none) |
| `50` | `--cpu N --cpu-load 50 --vm 1 --vm-bytes 128M` |
| `100` | `--cpu N --vm 1 --vm-bytes 256M --io 1` |

## Config C

Config C reads the Zephyr tick log from `SERIAL_DEV`. It keeps the first
`cycle,...` header line and the numeric rows after it, so boot messages are
dropped, and stops after `-n` rows. While the log is captured, the Linux
analytics workload (`pdm_bench`, with its own tick log discarded) and stress-ng
keep running, so the MPU carries the same load as in Configs A and B. The
Zephyr firmware is expected to print the same `cycle,wakeup_ns,period_ns,setpoint_pct`
columns as `pdm_bench`.

## `.meta` files

Each `.meta` file records the date, kernel, preemption model, CPU governor,
`sched_rt_runtime_us`, stress arguments and the exact command used. Having these
lets the report state exactly how each run was configured.
