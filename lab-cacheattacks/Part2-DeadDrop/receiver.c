#include "util.h"
#include <signal.h>
#include <sys/mman.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 21)
#define LINE_SIZE 64
#define PROBE_LINES 1024

#define SLOT_NS 45000000ULL
#define CALIBRATION_SLOTS 72
#define THRESHOLD_MARGIN 8

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

  uint32_t x = 0x5A5A5A5Au;
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
  uint64_t start = monotonic_ns();
  uint32_t avg = measure_avg_cycles(buf, perm, nlines);
  uint64_t elapsed = monotonic_ns() - start;
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

static uint32_t calibrate_threshold(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint32_t samples[CALIBRATION_SLOTS];
  for (int i = 0; i < CALIBRATION_SLOTS; i++) {
    samples[i] = sample_slot_cycles(buf, perm, nlines);
  }

  sort_u32(samples, CALIBRATION_SLOTS);
  uint32_t median = samples[CALIBRATION_SLOTS / 2];
  uint32_t p90 = samples[(CALIBRATION_SLOTS * 9) / 10];
  uint32_t threshold = p90 + THRESHOLD_MARGIN;

  printf("Receiver now listening.\n");
  printf("Idle median: %u, idle p90: %u, threshold: %u\n", median, p90, threshold);
  fflush(stdout);

  return threshold;
}

static inline int slot_is_active(char *buf, const uint32_t *perm, uint32_t nlines, uint32_t thr)
{
  return sample_slot_cycles(buf, perm, nlines) >= thr;
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
  uint32_t threshold = calibrate_threshold(buf, perm, PROBE_LINES);

  /* sync pattern: 1 1 0 0 1 1 */
  uint8_t shreg = 0;
  while (keep_running) {
    int active = slot_is_active(buf, perm, PROBE_LINES, threshold);
    shreg = (uint8_t)(((shreg << 1) | (active ? 1 : 0)) & 0x3Fu);
    if (shreg != 0x33u) {
      continue;
    }

    uint8_t value = 0;
    int valid = 1;
    for (int bit = 0; bit < 8; bit++) {
      int a = slot_is_active(buf, perm, PROBE_LINES, threshold);
      int b = slot_is_active(buf, perm, PROBE_LINES, threshold);

      if (a && !b) {
        value = (uint8_t)((value << 1) | 1u);
      } else if (!a && b) {
        value = (uint8_t)(value << 1);
      } else {
        valid = 0;
        break;
      }
    }

    if (!valid) {
      shreg = 0;
      continue;
    }

    printf("%u\n", (unsigned)value);
    fflush(stdout);

    int idle_slots = 0;
    while (keep_running && idle_slots < 3) {
      if (!slot_is_active(buf, perm, PROBE_LINES, threshold)) {
        idle_slots++;
      } else {
        idle_slots = 0;
      }
    }
    shreg = 0;
  }

  return 0;
}
