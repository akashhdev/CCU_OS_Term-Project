/*
 * pdm_bench.c - Dual-threaded Linux benchmark for Configurations A and B.
 *
 * Thread 1 (reflex):    periodic 10 ms control tick driven by
 *                       clock_nanosleep(TIMER_ABSTIME). Each cycle it only
 *                       samples the latest setpoint and records its wakeup
 *                       timestamp (Project Scope Statement, section 4.3.1).
 * Thread 2 (cognitive): periodic ~85 ms analytics loop. Runs a 1024-point
 *                       FFT over a vibration frame, classifies the bearing
 *                       condition and publishes a speed derating setpoint
 *                       through a "latest-value-wins" shared variable.
 *
 * Config A: run with --policy cfs  (default SCHED_OTHER)
 * Config B: run with --policy fifo (reflex thread SCHED_FIFO, priority 90)
 *
 * Timestamps are kept in preallocated memory and written as CSV only after
 * the run, so no file I/O happens inside the timed loop.
 *
 * CSV columns: cycle,wakeup_ns,period_ns,setpoint_pct,nivcsw,minflt
 *   wakeup_ns    CLOCK_MONOTONIC wakeup time of the cycle
 *   period_ns    wakeup-to-wakeup period (0 for the first cycle)
 *   setpoint_pct setpoint sampled in this cycle
 *   nivcsw       cumulative involuntary context switches of the reflex thread
 *   minflt       cumulative minor page faults of the reflex thread
 */
#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define FRAME_LEN 1024
#define NSEC_PER_SEC 1000000000LL
#define SAMPLE_RATE_HZ 12000.0

/* Fault classes and the prescriptive derating LUT (scope statement, 4.2). */
enum fault_class { CLASS_NORMAL, CLASS_BALL, CLASS_INNER, CLASS_OUTER, CLASS_COUNT };
static const char *const class_names[CLASS_COUNT] = { "normal", "ball", "inner", "outer" };
static const int derating_lut[CLASS_COUNT] = { 100, 85, 70, 60 };

struct options {
	long cycles;
	long period_us;
	long analytics_ms;
	int fifo;
	int rt_prio;
	int reflex_cpu;
	int analytics;
	int lock_memory;
	const char *out_path;
	const char *frames_path;
};

struct sample {
	int64_t wakeup_ns;
	int32_t setpoint;
	int32_t pad;
	int64_t nivcsw;
	int64_t minflt;
};

static atomic_int g_setpoint = 100;
static atomic_bool g_stop;
static atomic_long g_analytics_runs;
static atomic_long g_class_hist[CLASS_COUNT];

static struct options g_opt;
static struct sample *g_samples;

/* Vibration frames: either loaded from disk (float32, FRAME_LEN per frame)
 * or synthesized so the benchmark runs without the CWRU dataset. */
static float *g_frames;
static long g_nframes;

static inline int64_t ts_to_ns(const struct timespec *ts)
{
	return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

static inline void ts_add_ns(struct timespec *ts, int64_t ns)
{
	ts->tv_nsec += ns;
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static void die(const char *msg)
{
	perror(msg);
	exit(EXIT_FAILURE);
}

/* ------------------------------------------------------------------------ */
/* Analytics workload                                                        */
/* ------------------------------------------------------------------------ */

/* In-place iterative radix-2 FFT, n must be a power of two. */
static void fft(float *re, float *im, int n)
{
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (int len = 2; len <= n; len <<= 1) {
		double ang = -2.0 * M_PI / len;
		float wr = (float)cos(ang), wi = (float)sin(ang);
		for (int i = 0; i < n; i += len) {
			float cr = 1.0f, ci = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int a = i + k, b = i + k + len / 2;
				float xr = re[b] * cr - im[b] * ci;
				float xi = re[b] * ci + im[b] * cr;
				re[b] = re[a] - xr; im[b] = im[a] - xi;
				re[a] += xr;        im[a] += xi;
				float t = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = t;
			}
		}
	}
}

/* Characteristic defect frequencies of the CWRU drive-end bearing
 * (SKF 6205) at ~1797 rpm, in Hz. Used by the rule-based placeholder
 * classifier until the TFLite 1D-CNN (P3.5) replaces it. */
static const double defect_hz[CLASS_COUNT] = { 0.0, 141.0, 162.0, 107.0 };

static double band_energy(const float *mag, double f_lo, double f_hi)
{
	double bin_hz = SAMPLE_RATE_HZ / FRAME_LEN, e = 0.0;
	int lo = (int)(f_lo / bin_hz), hi = (int)(f_hi / bin_hz);
	if (lo < 1)
		lo = 1;
	if (hi > FRAME_LEN / 2)
		hi = FRAME_LEN / 2;
	for (int k = lo; k <= hi; k++)
		e += (double)mag[k] * mag[k];
	return e;
}

static enum fault_class classify(const float *frame)
{
	static float re[FRAME_LEN], im[FRAME_LEN], mag[FRAME_LEN / 2 + 1];

	/* Hann window + FFT. */
	for (int i = 0; i < FRAME_LEN; i++) {
		float w = 0.5f - 0.5f * (float)cos(2.0 * M_PI * i / (FRAME_LEN - 1));
		re[i] = frame[i] * w;
		im[i] = 0.0f;
	}
	fft(re, im, FRAME_LEN);
	double total = 0.0;
	for (int k = 0; k <= FRAME_LEN / 2; k++) {
		mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
		total += (double)mag[k] * mag[k];
	}
	if (total <= 0.0)
		return CLASS_NORMAL;

	/* Pick the defect band (with harmonics) holding the most energy; if no
	 * band clearly dominates, the bearing is considered healthy. */
	enum fault_class best = CLASS_NORMAL;
	double best_ratio = 0.0;
	for (int c = CLASS_BALL; c < CLASS_COUNT; c++) {
		double e = 0.0;
		for (int h = 1; h <= 3; h++)
			e += band_energy(mag, h * defect_hz[c] - 6.0, h * defect_hz[c] + 6.0);
		double ratio = e / total;
		if (ratio > best_ratio) {
			best_ratio = ratio;
			best = (enum fault_class)c;
		}
	}
	return best_ratio > 0.25 ? best : CLASS_NORMAL;
}

/* Synthetic frames: cycle through the four classes, each a mix of shaft
 * rotation, the class defect frequency and broadband noise. */
static void synthesize_frames(void)
{
	g_nframes = 64;
	g_frames = malloc(sizeof(float) * FRAME_LEN * g_nframes);
	if (!g_frames)
		die("malloc frames");
	unsigned seed = 12345;
	for (long f = 0; f < g_nframes; f++) {
		int c = (int)(f / 16) % CLASS_COUNT;
		for (int i = 0; i < FRAME_LEN; i++) {
			double t = i / SAMPLE_RATE_HZ;
			double v = 0.2 * sin(2.0 * M_PI * 29.95 * t);
			if (c != CLASS_NORMAL)
				for (int h = 1; h <= 3; h++)
					v += (1.0 / h) * sin(2.0 * M_PI * h * defect_hz[c] * t);
			seed = seed * 1103515245u + 12345u;
			v += 0.05 * (((seed >> 16) & 0x7fff) / 16384.0 - 1.0);
			g_frames[f * FRAME_LEN + i] = (float)v;
		}
	}
}

static void load_frames(const char *path)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		die(path);
	if (fseek(fp, 0, SEEK_END) != 0)
		die("fseek");
	long bytes = ftell(fp);
	rewind(fp);
	g_nframes = bytes / (long)(sizeof(float) * FRAME_LEN);
	if (g_nframes <= 0) {
		fprintf(stderr, "%s: need at least one %d-sample float32 frame\n", path, FRAME_LEN);
		exit(EXIT_FAILURE);
	}
	g_frames = malloc(sizeof(float) * FRAME_LEN * g_nframes);
	if (!g_frames)
		die("malloc frames");
	if (fread(g_frames, sizeof(float) * FRAME_LEN, g_nframes, fp) != (size_t)g_nframes)
		die("fread frames");
	fclose(fp);
}

static void *analytics_thread(void *arg)
{
	(void)arg;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	long frame = 0;

	while (!atomic_load(&g_stop)) {
		enum fault_class c = classify(&g_frames[(frame % g_nframes) * FRAME_LEN]);
		atomic_store(&g_setpoint, derating_lut[c]);
		atomic_fetch_add(&g_class_hist[c], 1);
		atomic_fetch_add(&g_analytics_runs, 1);
		frame++;

		ts_add_ns(&next, g_opt.analytics_ms * 1000000LL);
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Reflex (10 ms) control loop                                               */
/* ------------------------------------------------------------------------ */

static void *reflex_thread(void *arg)
{
	(void)arg;
	const int64_t period_ns = g_opt.period_us * 1000LL;
	struct timespec next, now;
	struct rusage ru;

	clock_gettime(CLOCK_MONOTONIC, &next);
	ts_add_ns(&next, period_ns);

	for (long k = 0; k < g_opt.cycles; k++) {
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
			;
		clock_gettime(CLOCK_MONOTONIC, &now);

		struct sample *s = &g_samples[k];
		s->wakeup_ns = ts_to_ns(&now);
		s->setpoint = atomic_load(&g_setpoint);
		getrusage(RUSAGE_THREAD, &ru);
		s->nivcsw = ru.ru_nivcsw;
		s->minflt = ru.ru_minflt;

		/* Advance on the absolute grid. If we overran by more than one
		 * period, skip the missed slots instead of firing back-to-back. */
		ts_add_ns(&next, period_ns);
		if (ts_to_ns(&next) <= s->wakeup_ns) {
			int64_t behind = s->wakeup_ns - ts_to_ns(&next);
			ts_add_ns(&next, (behind / period_ns + 1) * period_ns);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------ */
/* Setup and reporting                                                       */
/* ------------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options]\n"
		"  -n, --cycles N        control cycles to record (default 10000)\n"
		"  -p, --period-us US    control period in microseconds (default 10000)\n"
		"  -a, --analytics-ms MS analytics period in milliseconds (default 85)\n"
		"  -s, --policy P        cfs (Config A) or fifo (Config B) (default cfs)\n"
		"  -r, --priority N      SCHED_FIFO priority for the reflex thread (default 90)\n"
		"  -c, --cpu N           pin the reflex thread to CPU N (default: unpinned)\n"
		"  -f, --frames FILE     float32 vibration frames (1024 samples each)\n"
		"  -o, --out FILE        CSV output path (default stdout)\n"
		"  -A, --no-analytics    do not start the analytics thread\n"
		"  -m, --mlock           mlockall() to avoid demand paging\n"
		"  -h, --help            show this help\n",
		prog);
}

static void parse_args(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "cycles", required_argument, NULL, 'n' },
		{ "period-us", required_argument, NULL, 'p' },
		{ "analytics-ms", required_argument, NULL, 'a' },
		{ "policy", required_argument, NULL, 's' },
		{ "priority", required_argument, NULL, 'r' },
		{ "cpu", required_argument, NULL, 'c' },
		{ "frames", required_argument, NULL, 'f' },
		{ "out", required_argument, NULL, 'o' },
		{ "no-analytics", no_argument, NULL, 'A' },
		{ "mlock", no_argument, NULL, 'm' },
		{ "help", no_argument, NULL, 'h' },
		{ 0 }
	};

	g_opt = (struct options){
		.cycles = 10000, .period_us = 10000, .analytics_ms = 85,
		.rt_prio = 90, .reflex_cpu = -1, .analytics = 1,
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "n:p:a:s:r:c:f:o:Amh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n': g_opt.cycles = atol(optarg); break;
		case 'p': g_opt.period_us = atol(optarg); break;
		case 'a': g_opt.analytics_ms = atol(optarg); break;
		case 's':
			if (!strcmp(optarg, "fifo"))
				g_opt.fifo = 1;
			else if (strcmp(optarg, "cfs")) {
				fprintf(stderr, "unknown policy '%s'\n", optarg);
				exit(EXIT_FAILURE);
			}
			break;
		case 'r': g_opt.rt_prio = atoi(optarg); break;
		case 'c': g_opt.reflex_cpu = atoi(optarg); break;
		case 'f': g_opt.frames_path = optarg; break;
		case 'o': g_opt.out_path = optarg; break;
		case 'A': g_opt.analytics = 0; break;
		case 'm': g_opt.lock_memory = 1; break;
		case 'h': usage(argv[0]); exit(EXIT_SUCCESS);
		default: usage(argv[0]); exit(EXIT_FAILURE);
		}
	}
	if (g_opt.cycles <= 0 || g_opt.period_us <= 0 || g_opt.analytics_ms <= 0) {
		fprintf(stderr, "cycles, period and analytics period must be positive\n");
		exit(EXIT_FAILURE);
	}
}

static void write_csv(void)
{
	FILE *out = stdout;
	if (g_opt.out_path && !(out = fopen(g_opt.out_path, "w")))
		die(g_opt.out_path);

	fprintf(out, "cycle,wakeup_ns,period_ns,setpoint_pct,nivcsw,minflt\n");
	for (long k = 0; k < g_opt.cycles; k++) {
		const struct sample *s = &g_samples[k];
		int64_t period = k ? s->wakeup_ns - g_samples[k - 1].wakeup_ns : 0;
		fprintf(out, "%ld,%lld,%lld,%d,%lld,%lld\n", k, (long long)s->wakeup_ns,
			(long long)period, s->setpoint, (long long)s->nivcsw,
			(long long)s->minflt);
	}
	if (out != stdout)
		fclose(out);
}

static void print_summary(void)
{
	const double nominal_ms = g_opt.period_us / 1000.0;
	long n = 0, misses = 0, severe = 0;
	double sum = 0.0, sumsq = 0.0, worst = 0.0;

	for (long k = 1; k < g_opt.cycles; k++) {
		double t = (g_samples[k].wakeup_ns - g_samples[k - 1].wakeup_ns) / 1e6;
		sum += t;
		sumsq += t * t;
		if (t > worst)
			worst = t;
		misses += t > nominal_ms;
		severe += t > 50.0;
		n++;
	}
	if (!n)
		return;
	double mean = sum / n, var = sumsq / n - mean * mean;

	fprintf(stderr, "policy=%s periods=%ld mean=%.4f ms sigma=%.4f ms wcl=%.4f ms "
		"misses=%ld dmr=%.3f%% severe=%ld analytics_runs=%ld\n",
		g_opt.fifo ? "fifo" : "cfs", n, mean, var > 0 ? sqrt(var) : 0.0, worst,
		misses, 100.0 * misses / n, severe, atomic_load(&g_analytics_runs));
	fprintf(stderr, "classes:");
	for (int c = 0; c < CLASS_COUNT; c++)
		fprintf(stderr, " %s=%ld", class_names[c], atomic_load(&g_class_hist[c]));
	fprintf(stderr, "\n");
}

int main(int argc, char **argv)
{
	parse_args(argc, argv);

	if (g_opt.frames_path)
		load_frames(g_opt.frames_path);
	else
		synthesize_frames();

	g_samples = calloc(g_opt.cycles, sizeof(*g_samples));
	if (!g_samples)
		die("calloc samples");
	/* Touch every page up front so the timed loop does not demand-fault
	 * its own log buffer. */
	memset(g_samples, 0, g_opt.cycles * sizeof(*g_samples));

	if (g_opt.lock_memory && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
		die("mlockall");

	pthread_t reflex, analytics;
	pthread_attr_t attr;
	pthread_attr_init(&attr);

	if (g_opt.fifo) {
		struct sched_param sp = { .sched_priority = g_opt.rt_prio };
		pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
		pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
		pthread_attr_setschedparam(&attr, &sp);
	}
	if (g_opt.reflex_cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(g_opt.reflex_cpu, &set);
		pthread_attr_setaffinity_np(&attr, sizeof(set), &set);
	}

	if (g_opt.analytics && pthread_create(&analytics, NULL, analytics_thread, NULL) != 0)
		die("pthread_create analytics");

	int err = pthread_create(&reflex, &attr, reflex_thread, NULL);
	if (err) {
		errno = err;
		die(g_opt.fifo ? "pthread_create reflex (SCHED_FIFO needs root or CAP_SYS_NICE)"
			       : "pthread_create reflex");
	}
	pthread_attr_destroy(&attr);

	pthread_join(reflex, NULL);
	atomic_store(&g_stop, true);
	if (g_opt.analytics)
		pthread_join(analytics, NULL);

	write_csv();
	print_summary();
	free(g_samples);
	free(g_frames);
	return EXIT_SUCCESS;
}
