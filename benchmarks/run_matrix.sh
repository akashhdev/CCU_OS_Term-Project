#!/usr/bin/env bash
# run_matrix.sh - run the benchmark matrix of configurations x stress levels.
#
# Configurations (docs/Project_Scope_Statement.md §4.3.1):
#   A  Linux CFS                 mpu_linux/workload/pdm_bench --policy cfs
#   B  Linux SCHED_FIFO prio 90  mpu_linux/workload/pdm_bench --policy fifo
#   C  Dual-OS                   Zephyr tick log captured from SERIAL_DEV while
#                                the Linux analytics thread runs under stress
#
# Each run writes results/raw/<config>_<level>.csv plus a .meta file that
# records the kernel, CPU governor, and the exact commands used.
#
# Usage: benchmarks/run_matrix.sh [-c "A B C"] [-l "idle 50 100"] [-n cycles]
#                                 [-o outdir] [-r repeats] [-d]
#   -d  dry run: print the commands without running them
#
# Environment:
#   PDM_BENCH    path to the pdm_bench binary (default: mpu_linux/workload/pdm_bench)
#   BENCH_ARGS   extra pdm_bench arguments, e.g. "--mlock --cpu 3"
#   SERIAL_DEV   serial device that carries the Zephyr CSV log (Config C)
#   SERIAL_BAUD  baud rate for SERIAL_DEV (default 115200)
#   SETTLE_S     seconds to let stress-ng ramp up before measuring (default 5)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../mpu_linux/stress/stress_levels.sh
source "${ROOT}/mpu_linux/stress/stress_levels.sh"

CONFIGS="A B"
LEVELS="idle 50 100"
CYCLES=10000
OUTDIR="${ROOT}/results/raw"
REPEATS=1
DRY_RUN=0
PDM_BENCH="${PDM_BENCH:-${ROOT}/mpu_linux/workload/pdm_bench}"
BENCH_ARGS="${BENCH_ARGS:-}"
SERIAL_DEV="${SERIAL_DEV:-}"
SERIAL_BAUD="${SERIAL_BAUD:-115200}"
SETTLE_S="${SETTLE_S:-5}"
PERIOD_S=0.01

usage() {
	sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'
	exit "${1:-0}"
}

while getopts "c:l:n:o:r:dh" opt; do
	case "$opt" in
	c) CONFIGS="$OPTARG" ;;
	l) LEVELS="$OPTARG" ;;
	n) CYCLES="$OPTARG" ;;
	o) OUTDIR="$OPTARG" ;;
	r) REPEATS="$OPTARG" ;;
	d) DRY_RUN=1 ;;
	h) usage 0 ;;
	*) usage 1 ;;
	esac
done

log() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*" >&2; }

run() {
	log "+ $*"
	[[ $DRY_RUN -eq 1 ]] || "$@"
}

STRESS_PID=""
ANALYTICS_PID=""
cleanup() {
	for pid in $STRESS_PID $ANALYTICS_PID; do
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
	done
	STRESS_PID=""
	ANALYTICS_PID=""
}
trap cleanup EXIT INT TERM

start_stress() {
	local args
	args="$(stress_args "$1")"
	[[ -n $args ]] || return 0
	log "+ stress-ng $args --timeout 0 --quiet"
	if [[ $DRY_RUN -eq 0 ]]; then
		# shellcheck disable=SC2086
		stress-ng $args --timeout 0 --quiet &
		STRESS_PID=$!
		sleep "$SETTLE_S"
	fi
}

write_meta() {
	local meta="$1" config="$2" level="$3" cmd="$4"
	[[ $DRY_RUN -eq 1 ]] && return 0
	{
		echo "date=$(date -Is)"
		echo "host=$(hostname)"
		echo "kernel=$(uname -srvm)"
		echo "config=$config"
		echo "stress_level=$level"
		echo "stress_args=$(stress_args "$level")"
		echo "cycles=$CYCLES"
		echo "command=$cmd"
		echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
		echo "rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo n/a)"
		echo "preempt=$(uname -v | grep -o 'PREEMPT[_A-Z]*' || echo none)"
	} >"$meta"
}

run_linux() {
	local config="$1" level="$2" out="$3" policy
	[[ $config == A ]] && policy=cfs || policy=fifo
	local -a cmd=("$PDM_BENCH" --policy "$policy" --cycles "$CYCLES" --out "$out")
	# shellcheck disable=SC2206
	[[ -n $BENCH_ARGS ]] && cmd+=($BENCH_ARGS)
	[[ $policy == fifo && $EUID -ne 0 ]] && cmd=(sudo "${cmd[@]}")

	write_meta "${out%.csv}.meta" "$config" "$level" "${cmd[*]}"
	start_stress "$level"
	run "${cmd[@]}"
	cleanup
}

run_dual_os() {
	local level="$1" out="$2"
	if [[ -z $SERIAL_DEV ]]; then
		log "skipping Config C: set SERIAL_DEV to the Zephyr log port"
		return 0
	fi
	# Record a few extra seconds so the log holds at least $CYCLES ticks,
	# then keep only CSV lines (header plus numeric rows).
	local secs
	secs=$(awk -v n="$CYCLES" -v p="$PERIOD_S" 'BEGIN { printf "%d", n * p + 5 }')
	local cmd="stty -F $SERIAL_DEV $SERIAL_BAUD raw -echo && timeout $secs cat $SERIAL_DEV"

	write_meta "${out%.csv}.meta" C "$level" "$cmd"
	start_stress "$level"
	# The cognitive plane keeps running on Linux, as in Configs A and B.
	log "+ $PDM_BENCH --cycles $((CYCLES + 600)) --out /dev/null &  (analytics load; Linux tick log discarded)"
	if [[ $DRY_RUN -eq 0 ]]; then
		"$PDM_BENCH" --cycles $((CYCLES + 600)) --out /dev/null 2>/dev/null &
		ANALYTICS_PID=$!
	fi
	log "+ $cmd"
	if [[ $DRY_RUN -eq 0 ]]; then
		stty -F "$SERIAL_DEV" "$SERIAL_BAUD" raw -echo
		timeout "$secs" cat "$SERIAL_DEV" |
			tr -d '\r' |
			awk -v n="$CYCLES" '/^cycle,/ && !h { print; h = 1; next }
				/^[0-9]+,/ && h && c < n { print; c++ }' >"$out" || true
		log "captured $(($(wc -l <"$out") - 1)) Zephyr cycles"
	fi
	cleanup
}

if [[ $DRY_RUN -eq 0 ]]; then
	mkdir -p "$OUTDIR"
	command -v stress-ng >/dev/null || { log "stress-ng not found (apt install stress-ng)"; exit 1; }
	[[ " $CONFIGS " != *" A "* && " $CONFIGS " != *" B "* ]] || [[ -x $PDM_BENCH ]] ||
		{ log "$PDM_BENCH not built (make -C mpu_linux/workload)"; exit 1; }
fi

for ((rep = 1; rep <= REPEATS; rep++)); do
	for config in $CONFIGS; do
		for level in $LEVELS; do
			stress_args "$level" >/dev/null
			suffix=""
			[[ $REPEATS -gt 1 ]] && suffix="_r${rep}"
			out="${OUTDIR}/${config}_${level}${suffix}.csv"
			log "=== Config ${config}, stress ${level}, run ${rep}/${REPEATS} -> ${out}"
			case "$config" in
			A | B) run_linux "$config" "$level" "$out" ;;
			C) run_dual_os "$level" "$out" ;;
			*) log "unknown config '$config'"; exit 1 ;;
			esac
		done
	done
done
log "done"
