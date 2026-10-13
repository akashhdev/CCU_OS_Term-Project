#!/usr/bin/env bash
# stress_levels.sh - stress-ng argument sets for each benchmark stress level.
#
# Source this file, then call `stress_args <level>` to get the stress-ng
# arguments for idle, 50, or 100 (percent). Idle prints nothing, which
# means no stress-ng process is started.
#
# Set STRESS_CPUS, STRESS_VM_BYTES_50, or STRESS_VM_BYTES_100 in the
# environment to override the defaults.

STRESS_CPUS="${STRESS_CPUS:-$(nproc)}"
STRESS_VM_BYTES_50="${STRESS_VM_BYTES_50:-128M}"
STRESS_VM_BYTES_100="${STRESS_VM_BYTES_100:-256M}"

stress_args() {
	case "$1" in
	idle | 0)
		;;
	50)
		# Half load on every core plus moderate memory churn.
		echo "--cpu ${STRESS_CPUS} --cpu-load 50 --vm 1 --vm-bytes ${STRESS_VM_BYTES_50}"
		;;
	100)
		# Scope statement §8: stress-ng --cpu 4 --vm 1 (CPU count follows
		# the board), plus one I/O worker for kernel-side contention.
		echo "--cpu ${STRESS_CPUS} --vm 1 --vm-bytes ${STRESS_VM_BYTES_100} --io 1"
		;;
	*)
		echo "unknown stress level '$1' (expected idle, 50 or 100)" >&2
		return 1
		;;
	esac
}
