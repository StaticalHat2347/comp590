
#include"util.h"
// mman library to be used for hugepage allocations (e.g. mmap or posix_memalign only)
#include <sys/mman.h>
#include <signal.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1<<21)

#define CACHE_LINE_BYTES 64
#define NUM_SYMBOL_SETS 256
#define SET_STRIDE_BYTES (1 << 17)   // 128 KB keeps set index stable on more cache configs
#define PROBE_WAYS 12

#define CALIBRATION_ROUNDS 24
#define THRESHOLD_MARGIN 10
#define GAP_MARGIN 6

#define WINDOW_CYCLES 10
#define MIN_HITS_PER_WINDOW 5
#define REPEAT_COOLDOWN_WINDOWS 2

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig)
{
	(void)sig;
	keep_running = 0;
}

static inline volatile unsigned char *set_addr(void *buf, int set_idx, int way)
{
	return (volatile unsigned char *)((unsigned char *)buf +
	       (set_idx * CACHE_LINE_BYTES) + (way * SET_STRIDE_BYTES));
}

static void prime_all_sets(void *buf)
{
	static volatile unsigned char sink = 0;
	for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
		for (int way = 0; way < PROBE_WAYS; way++) {
			sink ^= *set_addr(buf, set_idx, way);
		}
	}
}

static CYCLES measure_set_latency(void *buf, int set_idx)
{
	uint64_t total = 0;
	for (int way = 0; way < PROBE_WAYS; way++) {
		total += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_idx, way));
	}
	return (CYCLES)(total / PROBE_WAYS);
}

static void one_probe_cycle(void *buf, int *best_set, CYCLES *best, CYCLES *second)
{
	prime_all_sets(buf);

	*best_set = -1;
	*best = 0;
	*second = 0;
	for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
		CYCLES lat = measure_set_latency(buf, set_idx);
		if (lat > *best) {
			*second = *best;
			*best = lat;
			*best_set = set_idx;
		} else if (lat > *second) {
			*second = lat;
		}
	}
}

static void calibrate_thresholds(void *buf, CYCLES *busy_threshold, CYCLES *gap_threshold)
{
	uint64_t sum_best = 0;
	uint64_t sum_gap = 0;
	for (int round = 0; round < CALIBRATION_ROUNDS; round++) {
		int best_set;
		CYCLES best, second;
		one_probe_cycle(buf, &best_set, &best, &second);
		(void)best_set;
		sum_best += best;
		sum_gap += (best > second) ? (best - second) : 0;
	}

	CYCLES avg_best = (CYCLES)(sum_best / CALIBRATION_ROUNDS);
	CYCLES avg_gap = (CYCLES)(sum_gap / CALIBRATION_ROUNDS);

	*busy_threshold = (CYCLES)(avg_best + THRESHOLD_MARGIN);
	*gap_threshold = (CYCLES)(avg_gap + GAP_MARGIN);
}

static int detect_hot_set(void *buf, CYCLES busy_threshold, CYCLES gap_threshold)
{
	int best_set;
	CYCLES best, second;
	one_probe_cycle(buf, &best_set, &best, &second);
	if (best_set < 0) {
		return -1;
	}
	if (best < busy_threshold) {
		return -1;
	}
	if ((best - second) < gap_threshold) {
		return -1;
	}

	return best_set;
}

int main(int argc, char **argv)
{
	void *buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE,
	                 MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB,
	                 -1, 0);
	if (buf == (void *)-1) {
		perror("mmap() error\n");
		exit(EXIT_FAILURE);
	}

	// Warm all addresses used during probing.
	for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
		for (int way = 0; way < PROBE_WAYS; way++) {
			*set_addr(buf, set_idx, way) = 1;
		}
	}

	printf("Please press enter.\n");

	char text_buf[2];
	fgets(text_buf, sizeof(text_buf), stdin);

	signal(SIGINT, handle_sigint);
	CYCLES busy_threshold = 0;
	CYCLES gap_threshold = 0;
	calibrate_thresholds(buf, &busy_threshold, &gap_threshold);

	printf("Receiver now listening.\n");
	printf("Busy threshold: %u cycles, gap threshold: %u cycles.\n",
	       busy_threshold, gap_threshold);

	int window_hits[NUM_SYMBOL_SETS] = {0};
	int nohit_count = 0;
	int cycle_count = 0;
	int cooldown_windows = 0;
	int last_printed = -1;

	while (keep_running) {
		int hot_set = detect_hot_set(buf, busy_threshold, gap_threshold);
		if (hot_set >= 0) {
			window_hits[hot_set]++;
		} else {
			nohit_count++;
		}

		cycle_count++;
		if (cycle_count < WINDOW_CYCLES) {
			continue;
		}

		int best_set = 0;
		for (int set_idx = 1; set_idx < NUM_SYMBOL_SETS; set_idx++) {
			if (window_hits[set_idx] > window_hits[best_set]) {
				best_set = set_idx;
			}
		}

		if (cooldown_windows > 0) {
			cooldown_windows--;
		}

		if (window_hits[best_set] >= MIN_HITS_PER_WINDOW && window_hits[best_set] > nohit_count) {
			if (best_set != last_printed || cooldown_windows == 0) {
				printf("Received: %d\n", best_set);
				fflush(stdout);
				last_printed = best_set;
				cooldown_windows = REPEAT_COOLDOWN_WINDOWS;
			}
		}

		memset(window_hits, 0, sizeof(window_hits));
		nohit_count = 0;
		cycle_count = 0;
	}

	printf("Receiver finished.\n");

	return 0;
}
