#include "util.h"
#include <sys/mman.h>
#include <signal.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 22)
#define CACHE_LINE_BYTES 64
#define WORKING_SET_LINES (BUFF_SIZE / CACHE_LINE_BYTES)

#define SLOT_NS 180000000ULL
#define PROBE_TOUCHES 8192

#define PREAMBLE_SLOTS 6
#define PREAMBLE_MIN_ACTIVE 5
#define GAP_SLOTS 2
#define GAP_MAX_ACTIVE 0
#define BIT_REPS 3

#define CALIBRATION_SAMPLES 50
#define MIN_MARGIN_NS 5000ULL
#define MAX_MARGIN_NS 60000ULL
#define NOISE_PAD_NS 4000ULL

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

static uint64_t probe_once_ns(volatile unsigned char *buf)
{
  static volatile unsigned char sink = 0;
  unsigned idx = 7;
  uint64_t start = monotonic_ns();

  for (int i = 0; i < PROBE_TOUCHES; i++) {
    idx = (idx * 1103515245u + 12345u) & (WORKING_SET_LINES - 1);
    sink ^= buf[idx * CACHE_LINE_BYTES];
  }

  return monotonic_ns() - start;
}

static uint64_t sample_slot_ns(volatile unsigned char *buf)
{
  uint64_t slot_start = monotonic_ns();
  uint64_t busy_ns = probe_once_ns(buf);
  uint64_t elapsed_ns = monotonic_ns() - slot_start;

  if (elapsed_ns < SLOT_NS) {
    sleep_ns(SLOT_NS - elapsed_ns);
  }

  return busy_ns;
}

static bool sample_slot_active(volatile unsigned char *buf, uint64_t active_threshold_ns)
{
  uint64_t busy_ns = sample_slot_ns(buf);
  return busy_ns >= active_threshold_ns;
}

int main(int argc, char **argv)
{
  int mmap_flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB;
  volatile unsigned char *buf =
      (volatile unsigned char *)mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
  if ((void *)buf == (void *)-1) {
    perror("mmap() error\n");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < BUFF_SIZE; i += CACHE_LINE_BYTES) {
    buf[i] = (unsigned char)(i & 0xFF);
  }

  printf("Please press enter.\n");
  char text_buf[2];
  fgets(text_buf, sizeof(text_buf), stdin);

  signal(SIGINT, handle_sigint);

  uint64_t sum_busy = 0;
  uint64_t max_busy = 0;
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    uint64_t busy_ns = sample_slot_ns(buf);
    sum_busy += busy_ns;
    if (busy_ns > max_busy) {
      max_busy = busy_ns;
    }
  }

  uint64_t baseline_ns = sum_busy / CALIBRATION_SAMPLES;
  uint64_t spread_ns = (max_busy > baseline_ns) ? (max_busy - baseline_ns) : 0;
  uint64_t margin_ns = (spread_ns / 3) + 6000ULL;
  if (margin_ns < MIN_MARGIN_NS) {
    margin_ns = MIN_MARGIN_NS;
  }
  if (margin_ns > MAX_MARGIN_NS) {
    margin_ns = MAX_MARGIN_NS;
  }

  uint64_t active_threshold_ns = baseline_ns + margin_ns;
  if (active_threshold_ns < max_busy + NOISE_PAD_NS) {
    active_threshold_ns = max_busy + NOISE_PAD_NS;
  }

  printf("Receiver now listening.\n");
  printf("Busy baseline: %llu ns, active threshold: %llu ns, max idle: %llu ns\n",
         (unsigned long long)baseline_ns,
         (unsigned long long)active_threshold_ns,
         (unsigned long long)max_busy);

  enum {
    WAIT_PREAMBLE = 0,
    WAIT_GAP = 1,
    READ_BITS = 2
  } state = WAIT_PREAMBLE;

  while (keep_running) {
    if (state == WAIT_PREAMBLE) {
      int active_cnt = 0;
      for (int i = 0; i < PREAMBLE_SLOTS; i++) {
        if (sample_slot_active(buf, active_threshold_ns)) {
          active_cnt++;
        }
      }

      if (active_cnt >= PREAMBLE_MIN_ACTIVE) {
        state = WAIT_GAP;
      }
      continue;
    }

    if (state == WAIT_GAP) {
      int gap_active = 0;
      for (int i = 0; i < GAP_SLOTS; i++) {
        if (sample_slot_active(buf, active_threshold_ns)) {
          gap_active++;
        }
      }

      if (gap_active <= GAP_MAX_ACTIVE) {
        state = READ_BITS;
      } else {
        state = WAIT_PREAMBLE;
      }
      continue;
    }

    if (state == READ_BITS) {
      uint8_t value = 0;

      for (int b = 0; b < 8; b++) {
        int bit_active = 0;
        for (int r = 0; r < BIT_REPS; r++) {
          if (sample_slot_active(buf, active_threshold_ns)) {
            bit_active++;
          }
        }
        int bit = (bit_active >= ((BIT_REPS + 1) / 2)) ? 1 : 0;
        value = (uint8_t)((value << 1) | bit);
      }

      printf("Received: %u\n", (unsigned)value);
      fflush(stdout);

      state = WAIT_PREAMBLE;
    }
  }

  printf("Receiver finished.\n");
  return 0;
}
