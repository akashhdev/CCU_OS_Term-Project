/*
 * Reflex-plane firmware for Configuration C (WBS P4.3).
 *
 * A periodic k_timer fires every CONFIG_APP_PERIOD_US and releases the
 * reflex thread. Each cycle the thread timestamps its wakeup, samples the
 * latest prescription setpoint and hands the sample to a low-priority
 * logger thread. The timed loop does no I/O, matching the Linux benchmark
 * (docs/Project_Scope_Statement.md section 4.3.1).
 *
 * The logger prints the same CSV columns as mpu_linux/workload/pdm_bench:
 *   cycle,wakeup_ns,period_ns,setpoint_pct
 * followed by a "# summary ..." line once CONFIG_APP_CYCLES samples are logged.
 * period_ns is measured in the reflex thread, so a sample dropped by a full
 * log queue leaves a gap in `cycle` without distorting the next period.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define PERIOD_US CONFIG_APP_PERIOD_US
#define CYCLES    CONFIG_APP_CYCLES

struct sample {
	uint32_t cycle;
	uint32_t setpoint;
	uint64_t wakeup_ns;
	uint64_t period_ns; /* 0 for the first cycle */
};

/* Latest-value-wins setpoint (percent of rated speed). The IPC receiver
 * (WBS P5.1) writes it with setpoint_update(); the reflex loop only reads. */
static atomic_t g_setpoint = ATOMIC_INIT(100);

void setpoint_update(int pct)
{
	if (pct >= 0 && pct <= 100) {
		atomic_set(&g_setpoint, pct);
	}
}

K_SEM_DEFINE(tick_sem, 0, 1);
K_MSGQ_DEFINE(sample_q, sizeof(struct sample), CONFIG_APP_LOG_QUEUE_DEPTH, 8);

static atomic_t g_dropped;
static atomic_t g_overruns;

static void tick_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	/* If the semaphore is already given, the reflex thread missed a
	 * whole period; count it so the log cannot hide an overrun. */
	if (k_sem_count_get(&tick_sem) != 0) {
		atomic_inc(&g_overruns);
	}
	k_sem_give(&tick_sem);
}

K_TIMER_DEFINE(tick_timer, tick_expiry, NULL);

/* 64-bit nanosecond clock built from the 32-bit hardware cycle counter.
 * Called at least every period, far below the counter wrap time. */
static uint64_t now_ns(void)
{
	static uint32_t last;
	static uint64_t total;
	uint32_t c = k_cycle_get_32();

	total += (uint32_t)(c - last);
	last = c;
	return k_cyc_to_ns_floor64(total);
}

static void reflex_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint64_t prev_ns = 0;

	(void)now_ns();
	k_timer_start(&tick_timer, K_USEC(PERIOD_US), K_USEC(PERIOD_US));

	for (uint32_t k = 0; CYCLES == 0 || k < CYCLES; k++) {
		k_sem_take(&tick_sem, K_FOREVER);

		uint64_t t = now_ns();
		struct sample s = {
			.cycle = k,
			.wakeup_ns = t,
			.period_ns = k ? t - prev_ns : 0,
			.setpoint = (uint32_t)atomic_get(&g_setpoint),
		};
		prev_ns = t;

		/* Apply the setpoint to the actuator here (PWM duty in a real
		 * rig; out of scope per section 4.2). */

		if (k_msgq_put(&sample_q, &s, K_NO_WAIT) != 0) {
			atomic_inc(&g_dropped);
		}
	}
	k_timer_stop(&tick_timer);
}

K_THREAD_DEFINE(reflex_tid, 1024, reflex_thread, NULL, NULL, NULL,
		CONFIG_APP_REFLEX_PRIORITY, 0, 0);

int main(void)
{
	struct sample s;
	uint64_t sum_ns = 0, max_ns = 0, min_ns = UINT64_MAX;
	uint32_t logged = 0, periods = 0, misses = 0;
	const uint64_t nominal_ns = (uint64_t)PERIOD_US * 1000U;

	/* main() is the logger thread. */
	k_thread_priority_set(k_current_get(), CONFIG_APP_LOGGER_PRIORITY);

	printk("# reflex_tick period_us=%d cycles=%d board=%s\n", PERIOD_US, CYCLES,
	       CONFIG_BOARD);
	printk("cycle,wakeup_ns,period_ns,setpoint_pct\n");

	while (CYCLES == 0 || logged < CYCLES) {
		if (k_msgq_get(&sample_q, &s, K_MSEC(1000)) != 0) {
			if (k_thread_join(reflex_tid, K_NO_WAIT) == 0) {
				break; /* reflex done and queue drained */
			}
			continue;
		}

		printk("%u,%llu,%llu,%u\n", s.cycle, (unsigned long long)s.wakeup_ns,
		       (unsigned long long)s.period_ns, s.setpoint);
		if (s.period_ns) {
			sum_ns += s.period_ns;
			max_ns = MAX(max_ns, s.period_ns);
			min_ns = MIN(min_ns, s.period_ns);
			misses += s.period_ns > nominal_ns;
			periods++;
		}
		logged++;
	}

	if (periods) {
		printk("# summary periods=%u mean_ns=%llu min_ns=%llu max_ns=%llu "
		       "misses=%u overruns=%ld dropped=%ld\n",
		       periods, (unsigned long long)(sum_ns / periods),
		       (unsigned long long)min_ns, (unsigned long long)max_ns, misses,
		       (long)atomic_get(&g_overruns), (long)atomic_get(&g_dropped));
	}
	return 0;
}
