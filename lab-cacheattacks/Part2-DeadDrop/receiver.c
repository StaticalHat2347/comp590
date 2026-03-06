#include "util.h"
#include <sys/mman.h>
#include <signal.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 23)
#define LINE_SIZE 64
#define PROBE_LINES 2048

#define SLOT_NS 60000000ULL
#define CALIBRATION_SLOTS 64
#define THRESHOLD_MARGIN 3

#define PREAMBLE_SLOTS 10
#define PREAMBLE_MIN_ACTIVE 7
#define GAP_SLOTS 2
#define GAP_MAX_ACTIVE 1
#define BIT_REP 5

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

static void *alloc_buffer(void)
{
  int flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB;
  void *buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
  if (buf == (void *)-1 && MAP_HUGETLB != 0) {
    flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE;
    buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
  }
  if (buf == (void *)-1) {
    perror("mmap");
    exit(EXIT_FAILURE);
  }
  return buf;
}

static void build_perm(uint32_t *idx, uint32_t n)
{
  for (uint32_t i = 0; i < n; i++) {
    idx[i] = i;
  }

  uint32_t x = 0x12345678u;
  for (uint32_t i = n - 1; i > 0; i--) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    uint32_t j = x % (i + 1);
    uint32_t t = idx[i];
    idx[i] = idx[j];
    idx[j] = t;
  }
}

static uint32_t measure_avg_cycles(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t sum = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    char *p = buf + ((size_t)perm[k] * LINE_SIZE);
    sum += measure_one_block_access_time((ADDR_PTR)p);
  }
  return (uint32_t)(sum / nlines);
}

static uint32_t sample_slot_cycles(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t slot_start = monotonic_ns();
  uint32_t avg = measure_avg_cycles(buf, perm, nlines);
  uint64_t elapsed = monotonic_ns() - slot_start;
  if (elapsed < SLOT_NS) {
    sleep_ns(SLOT_NS - elapsed);
  }
  return avg;
}

static void sort_u32(uint32_t *arr, int n)
{
  for (int i = 1; i < n; i++) {
    uint32_t key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

static uint32_t calibrate_threshold(
    char *buf, const uint32_t *perm, uint32_t nlines, uint32_t *min_threshold_out)
{
  uint32_t samples[CALIBRATION_SLOTS];
  for (int i = 0; i < CALIBRATION_SLOTS; i++) {
    samples[i] = sample_slot_cycles(buf, perm, nlines);
  }

  sort_u32(samples, CALIBRATION_SLOTS);
  uint32_t median = samples[CALIBRATION_SLOTS / 2];
  uint32_t p75 = samples[(CALIBRATION_SLOTS * 3) / 4];
  uint32_t p90 = samples[(CALIBRATION_SLOTS * 9) / 10];
  uint32_t threshold = p75 + THRESHOLD_MARGIN;
  uint32_t min_threshold = p75 + 2;
  *min_threshold_out = min_threshold;

  printf("Receiver now listening.\n");
  printf("Idle median: %u, idle p75: %u, idle p90: %u, active threshold: %u\n",
         median, p75, p90, threshold);
  fflush(stdout);

  return threshold;
}

static inline bool is_active(uint32_t avg_cycles, uint32_t threshold)
{
  return avg_cycles >= threshold;
}

int main(void)
{
  char *buf = (char *)alloc_buffer();
  for (int i = 0; i < BUFF_SIZE; i += LINE_SIZE) {
    buf[i] = (char)(i & 0xFF);
  }

  uint32_t *perm = (uint32_t *)malloc(PROBE_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, PROBE_LINES);

  printf("Please press enter.\n");
  char tmp[8];
  fgets(tmp, sizeof(tmp), stdin);

  signal(SIGINT, handle_sigint);
  uint32_t min_threshold = 0;
  uint32_t threshold = calibrate_threshold(buf, perm, PROBE_LINES, &min_threshold);
  int idle_windows = 0;

  while (keep_running) {
    int preamble_active = 0;
    for (int i = 0; i < PREAMBLE_SLOTS; i++) {
      uint32_t avg = sample_slot_cycles(buf, perm, PROBE_LINES);
      if (is_active(avg, threshold)) {
        preamble_active++;
      }
    }
    if (preamble_active < PREAMBLE_MIN_ACTIVE) {
      idle_windows++;
      if ((idle_windows % 8) == 0 && threshold > min_threshold) {
        threshold--;
      }
      continue;
    }
    idle_windows = 0;

    int gap_active = 0;
    for (int i = 0; i < GAP_SLOTS; i++) {
      uint32_t avg = sample_slot_cycles(buf, perm, PROBE_LINES);
      if (is_active(avg, threshold)) {
        gap_active++;
      }
    }
    if (gap_active > GAP_MAX_ACTIVE) {
      continue;
    }

    uint8_t value = 0;
    for (int b = 0; b < 8; b++) {
      int ones = 0;
      for (int r = 0; r < BIT_REP; r++) {
        uint32_t avg = sample_slot_cycles(buf, perm, PROBE_LINES);
        if (is_active(avg, threshold)) {
          ones++;
        }
      }
      int bit = (ones * 2 >= BIT_REP) ? 1 : 0;
      value = (uint8_t)((value << 1) | bit);
    }

    printf("Received: %u\n", (unsigned)value);
    fflush(stdout);
  }

  return 0;
}
