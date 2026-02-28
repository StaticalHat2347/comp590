
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
#define SET_STRIDE_BYTES (1 << 16)   // 64 KB keeps the same L2 set index
#define PROBE_WAYS 24

#define FRAME_HEADER_SET 255
#define MIN_CONFIDENCE_GAP 15
#define CALIBRATION_ROUNDS 20
#define GAP_CONFIRM_SCANS 4
#define MIN_SYMBOL_VOTES 3

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

static CYCLES measure_set_latency(void *buf, int set_idx)
{
	uint64_t total = 0;
	for (int way = 0; way < PROBE_WAYS; way++) {
		total += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_idx, way));
	}
	return (CYCLES)(total / PROBE_WAYS);
}

static CYCLES calibrate_busy_threshold(void *buf)
{
	uint64_t sum_all = 0;
	uint64_t count = 0;

	for (int round = 0; round < CALIBRATION_ROUNDS; round++) {
		for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
			CYCLES lat = measure_set_latency(buf, set_idx);
			sum_all += lat;
			count++;
		}
	}

	if (count == 0) {
		return 0;
	}

	CYCLES baseline = (CYCLES)(sum_all / count);
	return (CYCLES)(baseline + 25);
}

static int detect_hot_set(void *buf, CYCLES busy_threshold)
{
	CYCLES best = 0;
	CYCLES second = 0;
	int best_set = -1;

	for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
		CYCLES lat = measure_set_latency(buf, set_idx);
		if (lat > best) {
			second = best;
			best = lat;
			best_set = set_idx;
		} else if (lat > second) {
			second = lat;
		}
	}

	if (best_set < 0) {
		return -1;
	}
	if (best < busy_threshold) {
		return -1;
	}
	if ((best - second) < MIN_CONFIDENCE_GAP) {
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
	CYCLES busy_threshold = calibrate_busy_threshold(buf);

	printf("Receiver now listening.\n");
	printf("Busy threshold: %u cycles.\n", busy_threshold);

	bool waiting_for_payload = false;
	bool collecting_symbol = false;
	int votes[NUM_SYMBOL_SETS] = {0};
	int total_votes = 0;
	int idle_scans = 0;

	while (keep_running) {
		int hot_set = detect_hot_set(buf, busy_threshold);
		if (hot_set >= 0) {
			if (!collecting_symbol) {
				memset(votes, 0, sizeof(votes));
				total_votes = 0;
				idle_scans = 0;
				collecting_symbol = true;
			}
			votes[hot_set]++;
			total_votes++;
			idle_scans = 0;
			continue;
		}

		if (!collecting_symbol) {
			continue;
		}

		idle_scans++;
		if (idle_scans < GAP_CONFIRM_SCANS) {
			continue;
		}

		int best_set = 0;
		for (int set_idx = 1; set_idx < NUM_SYMBOL_SETS; set_idx++) {
			if (votes[set_idx] > votes[best_set]) {
				best_set = set_idx;
			}
		}

		if (total_votes >= MIN_SYMBOL_VOTES && (votes[best_set] * 2) >= total_votes) {
			if (waiting_for_payload) {
				printf("Received: %d\n", best_set);
				fflush(stdout);
				waiting_for_payload = false;
			} else if (best_set == FRAME_HEADER_SET) {
				waiting_for_payload = true;
			}
		}

		collecting_symbol = false;
		idle_scans = 0;
		total_votes = 0;

	}

	printf("Receiver finished.\n");

	return 0;
}
