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
#define PROBE_WAYS 32

#define MARKER_SET 27

#define SLOT_NS 180000000ULL
#define PRIME_FRACTION_NUM 3
#define PRIME_FRACTION_DEN 4

#define CALIBRATION_SAMPLES 120
#define LATENCY_MARGIN 4

#define SYNC_ACTIVE_SLOTS 10
#define SYNC_GAP_SLOTS 3
#define PRE_DATA_GUARD_SLOTS 1
#define SYNC_MIN_ACTIVE 8

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int sig)
{
  (void)sig;
  keep_running = 0;
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

static void prime_set(void *buf, int set_idx)
{
  static volatile unsigned char sink = 0;
  for (int way = 0; way < PROBE_WAYS; way++) {
    sink ^= *set_addr(buf, set_idx, way);
  }
}

static CYCLES probe_set_latency(void *buf, int set_idx)
{
  uint64_t sum = 0;
  for (int way = 0; way < PROBE_WAYS; way++) {
    sum += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_idx, way));
  }
  return (CYCLES)(sum / PROBE_WAYS);
}

static CYCLES sample_slot_latency(void *buf)
{
  prime_set(buf, MARKER_SET);
  sleep_ns((SLOT_NS * PRIME_FRACTION_NUM) / PRIME_FRACTION_DEN);
  CYCLES lat = probe_set_latency(buf, MARKER_SET);
  uint64_t rem = SLOT_NS - ((SLOT_NS * PRIME_FRACTION_NUM) / PRIME_FRACTION_DEN);
  sleep_ns(rem);
  return lat;
}

static bool sample_slot_active(void *buf, CYCLES threshold)
{
  return sample_slot_latency(buf) >= threshold;
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

  for (int set_idx = 0; set_idx < 64; set_idx++) {
    for (int way = 0; way < PROBE_WAYS; way++) {
      *set_addr(buf, set_idx, way) = 1;
    }
  }

  printf("Please press enter.\n");
  char text_buf[2];
  fgets(text_buf, sizeof(text_buf), stdin);

  signal(SIGINT, handle_sigint);

  uint64_t sum = 0;
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    prime_set(buf, MARKER_SET);
    CYCLES lat = probe_set_latency(buf, MARKER_SET);
    sum += lat;
    sleep_ns(1000000ULL);
  }

  CYCLES baseline = (CYCLES)(sum / CALIBRATION_SAMPLES);
  CYCLES active_threshold = (CYCLES)(baseline + LATENCY_MARGIN);

  printf("Receiver now listening.\n");
  printf("Latency baseline: %u, active threshold: %u\n", baseline, active_threshold);

  int active_run = 0;
  int inactive_run = 0;

  enum {
    WAIT_SYNC = 0,
    WAIT_SYNC_GAP = 1,
    READ_BITS = 2
  } state = WAIT_SYNC;

  while (keep_running) {
    if (state == WAIT_SYNC) {
      int active_cnt = 0;
      uint64_t sync_sum = 0;
      for (int i = 0; i < SYNC_ACTIVE_SLOTS; i++) {
        CYCLES lat = sample_slot_latency(buf);
        sync_sum += lat;
        if (lat >= active_threshold) {
          active_cnt++;
        }
      }

      if (active_cnt >= SYNC_MIN_ACTIVE) {
        CYCLES sync_avg = (CYCLES)(sync_sum / SYNC_ACTIVE_SLOTS);
        // Adaptive threshold between idle baseline and measured sync intensity.
        active_threshold = (CYCLES)((baseline + sync_avg) / 2);
        state = WAIT_SYNC_GAP;
        inactive_run = 0;
      }
      continue;
    }

    if (state == WAIT_SYNC_GAP) {
      bool active = sample_slot_active(buf, active_threshold);
      if (!active) {
        inactive_run++;
      } else {
        inactive_run = 0;
      }

      if (inactive_run >= SYNC_GAP_SLOTS) {
        state = READ_BITS;
      } else if (inactive_run == 0 && active) {
        // If sync gap is not observed, fallback to searching sync.
        state = WAIT_SYNC;
        active_run = 0;
      }
      continue;
    }

    if (state == READ_BITS) {
      // Skip guard idle slots between sync and payload to avoid
      // classifying residual sync activity as the first data bit.
      for (int g = 0; g < PRE_DATA_GUARD_SLOTS; g++) {
        (void)sample_slot_active(buf, active_threshold);
      }

      uint8_t value = 0;
      for (int b = 0; b < 8; b++) {
        bool active = sample_slot_active(buf, active_threshold);
        value = (uint8_t)((value << 1) | (active ? 1 : 0));
      }

      printf("Received: %u\n", (unsigned)value);
      fflush(stdout);

      state = WAIT_SYNC;
      active_run = 0;
      inactive_run = 0;
      continue;
    }
  }

  printf("Receiver finished.\n");
  return 0;
}
