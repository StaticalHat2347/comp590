#include "util.h"
#include <sys/mman.h>
#include <signal.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 21)

#define CACHE_LINE_BYTES 64
#define SET_STRIDE_BYTES (1 << 14)
#define PROBE_WAYS 96
#define LOW_SET_COUNT 64
#define PAIR_OFFSET 64

#define CALIBRATION_ROUNDS 24
#define BUSY_MARGIN 8
#define GAP_MARGIN 4

#define PRIME_TO_PROBE_WAIT_NS 40000

#define START_PULSE_NS 800000000ULL
#define GAP_NS 300000000ULL
#define PAYLOAD_BASE_NS 400000000ULL
#define PAYLOAD_STEP_NS 12000000ULL

#define START_TOL_NS 300000000ULL
#define GAP_TOL_NS 250000000ULL
#define PAYLOAD_TOL_NS 200000000ULL

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig)
{
  (void)sig;
  keep_running = 0;
}

static inline uint64_t monotonic_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline volatile unsigned char *set_addr(void *buf, int set_idx, int way)
{
  return (volatile unsigned char *)((unsigned char *)buf +
         (set_idx * CACHE_LINE_BYTES) + (way * SET_STRIDE_BYTES));
}

static void prime_all_pairs(void *buf)
{
  static volatile unsigned char sink = 0;
  for (int i = 0; i < LOW_SET_COUNT; i++) {
    int a = i;
    int b = i + PAIR_OFFSET;
    for (int way = 0; way < PROBE_WAYS; way++) {
      sink ^= *set_addr(buf, a, way);
      sink ^= *set_addr(buf, b, way);
    }
  }
}

static CYCLES measure_pair_latency(void *buf, int low_set_idx)
{
  uint64_t sum_a = 0;
  uint64_t sum_b = 0;
  int a = low_set_idx;
  int b = low_set_idx + PAIR_OFFSET;
  for (int way = 0; way < PROBE_WAYS; way++) {
    sum_a += measure_one_block_access_time((ADDR_PTR)set_addr(buf, a, way));
    sum_b += measure_one_block_access_time((ADDR_PTR)set_addr(buf, b, way));
  }
  CYCLES avg_a = (CYCLES)(sum_a / PROBE_WAYS);
  CYCLES avg_b = (CYCLES)(sum_b / PROBE_WAYS);
  return (avg_a > avg_b) ? avg_a : avg_b;
}

static void detect_best_pair(void *buf, int *best_idx, CYCLES *best, CYCLES *second)
{
  prime_all_pairs(buf);
  struct timespec ts = {.tv_sec = 0, .tv_nsec = PRIME_TO_PROBE_WAIT_NS};
  nanosleep(&ts, NULL);

  *best_idx = -1;
  *best = 0;
  *second = 0;
  for (int i = 0; i < LOW_SET_COUNT; i++) {
    CYCLES lat = measure_pair_latency(buf, i);
    if (lat > *best) {
      *second = *best;
      *best = lat;
      *best_idx = i;
    } else if (lat > *second) {
      *second = lat;
    }
  }
}

static void calibrate_thresholds(void *buf, CYCLES *busy_threshold, CYCLES *gap_threshold)
{
  uint64_t sum_best = 0;
  uint64_t sum_gap = 0;
  for (int i = 0; i < CALIBRATION_ROUNDS; i++) {
    int idx;
    CYCLES best, second;
    detect_best_pair(buf, &idx, &best, &second);
    (void)idx;
    sum_best += best;
    sum_gap += (best > second) ? (best - second) : 0;
  }

  CYCLES avg_best = (CYCLES)(sum_best / CALIBRATION_ROUNDS);
  CYCLES avg_gap = (CYCLES)(sum_gap / CALIBRATION_ROUNDS);
  *busy_threshold = (CYCLES)(avg_best + BUSY_MARGIN);
  *gap_threshold = (CYCLES)(avg_gap + GAP_MARGIN);
}

static bool in_range_u64(uint64_t x, uint64_t center, uint64_t tol)
{
  uint64_t lo = (center > tol) ? (center - tol) : 0;
  uint64_t hi = center + tol;
  return x >= lo && x <= hi;
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

  for (int set_idx = 0; set_idx < 128; set_idx++) {
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

  enum {
    WAIT_START_PULSE = 0,
    WAIT_GAP_AFTER_START = 1,
    WAIT_PAYLOAD_PULSE = 2
  } state = WAIT_START_PULSE;

  bool currently_hot = false;
  uint64_t hot_start_ns = 0;
  uint64_t last_hot_end_ns = 0;

  while (keep_running) {
    int best_idx;
    CYCLES best, second;
    detect_best_pair(buf, &best_idx, &best, &second);

    bool hot = (best_idx >= 0) && (best >= busy_threshold) && ((best - second) >= gap_threshold);
    uint64_t now = monotonic_ns();

    if (hot && !currently_hot) {
      currently_hot = true;
      hot_start_ns = now;
      continue;
    }

    if (!hot && currently_hot) {
      currently_hot = false;
      uint64_t pulse_ns = now - hot_start_ns;
      last_hot_end_ns = now;

      if (state == WAIT_START_PULSE) {
        if (in_range_u64(pulse_ns, START_PULSE_NS, START_TOL_NS)) {
          state = WAIT_GAP_AFTER_START;
        }
      } else if (state == WAIT_PAYLOAD_PULSE) {
        if (pulse_ns + PAYLOAD_TOL_NS >= PAYLOAD_BASE_NS) {
          int64_t delta = (int64_t)pulse_ns - (int64_t)PAYLOAD_BASE_NS;
          int decoded = (int)((delta + (int64_t)(PAYLOAD_STEP_NS / 2)) / (int64_t)PAYLOAD_STEP_NS);
          if (decoded < 0) decoded = 0;
          if (decoded > 255) decoded = 255;
          printf("Received: %d\n", decoded);
          fflush(stdout);
        }
        state = WAIT_START_PULSE;
      }
      continue;
    }

    if (state == WAIT_GAP_AFTER_START && !currently_hot) {
      uint64_t gap_ns = now - last_hot_end_ns;
      if (in_range_u64(gap_ns, GAP_NS, GAP_TOL_NS)) {
        state = WAIT_PAYLOAD_PULSE;
      } else if (gap_ns > (GAP_NS + GAP_TOL_NS)) {
        state = WAIT_START_PULSE;
      }
    }
  }

  printf("Receiver finished.\n");
  return 0;
}
