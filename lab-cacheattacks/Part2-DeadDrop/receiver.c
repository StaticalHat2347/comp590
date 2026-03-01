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
#define SET_STRIDE_BYTES (1 << 16)
#define PROBE_WAYS 20

#define MARKER_SET 27
#define CONTROL_SET 11
#define PAIR_OFFSET 64

#define CALIBRATION_SAMPLES 200
#define ACTIVE_MARGIN 2
#define SAMPLE_NS 2000000ULL

#define SYNC_NS 1500000000ULL
#define SYNC_GAP_NS 900000000ULL
#define BIT_SLOT_NS 500000000ULL

#define SYNC_DETECT_NS 500000000ULL
#define SYNC_GAP_DETECT_NS 850000000ULL
#define SYNC_ACTIVE_PCT 60
#define GAP_INACTIVE_PCT 80
#define BIT_ACTIVE_PCT 65

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

static inline void sleep_ns(uint64_t ns)
{
  struct timespec ts;
  ts.tv_sec = (time_t)(ns / 1000000000ULL);
  ts.tv_nsec = (long)(ns % 1000000000ULL);
  nanosleep(&ts, NULL);
}

static inline volatile unsigned char *set_addr(void *buf, int set_idx, int way)
{
  return (volatile unsigned char *)((unsigned char *)buf +
         (set_idx * CACHE_LINE_BYTES) + (way * SET_STRIDE_BYTES));
}

static CYCLES measure_pair_latency(void *buf, int set_idx)
{
  uint64_t sum_a = 0;
  uint64_t sum_b = 0;
  int paired = set_idx + PAIR_OFFSET;
  for (int way = 0; way < PROBE_WAYS; way++) {
    sum_a += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_idx, way));
    sum_b += measure_one_block_access_time((ADDR_PTR)set_addr(buf, paired, way));
  }
  CYCLES avg_a = (CYCLES)(sum_a / PROBE_WAYS);
  CYCLES avg_b = (CYCLES)(sum_b / PROBE_WAYS);
  return (avg_a > avg_b) ? avg_a : avg_b;
}

static int measure_diff(void *buf)
{
  CYCLES marker = measure_pair_latency(buf, MARKER_SET);
  CYCLES control = measure_pair_latency(buf, CONTROL_SET);
  return (int)marker - (int)control;
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

  int baseline_sum = 0;
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    baseline_sum += measure_diff(buf);
    sleep_ns(SAMPLE_NS);
  }
  int diff_baseline = baseline_sum / CALIBRATION_SAMPLES;
  int active_threshold = diff_baseline + ACTIVE_MARGIN;

  printf("Receiver now listening.\n");
  printf("Diff baseline: %d, active threshold: %d\n", diff_baseline, active_threshold);

  enum {
    WAIT_SYNC = 0,
    WAIT_SYNC_GAP = 1,
    READ_BITS = 2
  } state = WAIT_SYNC;

  uint64_t window_start = 0;
  int active_samples = 0;
  int total_samples = 0;

  while (keep_running) {
    int diff = measure_diff(buf);
    bool active = (diff >= active_threshold);
    uint64_t now = monotonic_ns();

    if (state == WAIT_SYNC) {
      if (window_start == 0) {
        window_start = now;
        active_samples = 0;
        total_samples = 0;
      }

      total_samples++;
      if (active) {
        active_samples++;
      }

      if ((now - window_start) >= SYNC_DETECT_NS) {
        if (total_samples > 0 &&
            (active_samples * 100) >= (SYNC_ACTIVE_PCT * total_samples)) {
          state = WAIT_SYNC_GAP;
        }
        window_start = 0;
      }
      sleep_ns(SAMPLE_NS);
      continue;
    }

    if (state == WAIT_SYNC_GAP) {
      if (window_start == 0) {
        window_start = now;
        active_samples = 0;
        total_samples = 0;
      }

      total_samples++;
      if (active) {
        active_samples++;
      }

      if ((now - window_start) >= SYNC_GAP_DETECT_NS) {
        int inactive_samples = total_samples - active_samples;
        if (total_samples > 0 &&
            (inactive_samples * 100) >= (GAP_INACTIVE_PCT * total_samples)) {
          state = READ_BITS;
        } else {
          state = WAIT_SYNC;
        }
        window_start = 0;
      }
      sleep_ns(SAMPLE_NS);
      continue;
    }

    if (state == READ_BITS) {
      uint8_t value = 0;
      for (int b = 0; b < 8; b++) {
        uint64_t slot_start = monotonic_ns();
        int active_cnt = 0;
        int total_cnt = 0;

        while ((monotonic_ns() - slot_start) < BIT_SLOT_NS) {
          int slot_diff = measure_diff(buf);
          if (slot_diff >= active_threshold) {
            active_cnt++;
          }
          total_cnt++;
          sleep_ns(SAMPLE_NS);
        }

        int bit = ((active_cnt * 100) >= (BIT_ACTIVE_PCT * total_cnt)) ? 1 : 0;
        value = (uint8_t)((value << 1) | (uint8_t)bit);
      }

      printf("Received: %u\n", (unsigned)value);
      fflush(stdout);

      state = WAIT_SYNC;
      window_start = 0;
      active_samples = 0;
      total_samples = 0;
      continue;
    }
  }

  printf("Receiver finished.\n");
  return 0;
}
