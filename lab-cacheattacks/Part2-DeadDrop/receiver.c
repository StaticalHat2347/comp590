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
#define PROBE_TOUCHES 4096

#define PREAMBLE_SLOTS 10
#define PREAMBLE_MIN_ACTIVE 9
#define GAP_SLOTS 2
#define GAP_MAX_ACTIVE 1
#define BIT_REPS 3

#define CALIBRATION_SAMPLES 50
#define MIN_MARGIN_CYCLES 50000ULL
#define MAX_MARGIN_CYCLES 300000ULL
#define NOISE_PAD_CYCLES 20000ULL
#define DIFF_MIN_CONFIDENCE_FLOOR 80000ULL
#define DIFF_MIN_CONFIDENCE_CEIL 800000ULL

typedef enum {
  RX_MODE_BYTE = 0,
  RX_MODE_SINGLE_BIT = 1,
  RX_MODE_SINGLE_BIT_DIFF = 2
} rx_mode_t;

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

static uint64_t probe_once_cycles(volatile unsigned char *buf)
{
  unsigned idx = 7;
  uint64_t total_cycles = 0;

  for (int i = 0; i < PROBE_TOUCHES; i++) {
    idx = (idx * 1103515245u + 12345u) & (WORKING_SET_LINES - 1);
    total_cycles += (uint64_t)measure_one_block_access_time((ADDR_PTR)(buf + idx * CACHE_LINE_BYTES));
  }

  return total_cycles;
}

static uint64_t sample_slot_cycles(volatile unsigned char *buf)
{
  uint64_t slot_start = monotonic_ns();
  uint64_t busy_cycles = probe_once_cycles(buf);
  uint64_t elapsed_ns = monotonic_ns() - slot_start;

  if (elapsed_ns < SLOT_NS) {
    sleep_ns(SLOT_NS - elapsed_ns);
  }

  return busy_cycles;
}

static bool sample_slot_active(volatile unsigned char *buf, uint64_t active_threshold_cycles)
{
  uint64_t busy_cycles = sample_slot_cycles(buf);
  return busy_cycles >= active_threshold_cycles;
}

static void sort_u64(uint64_t *arr, int n)
{
  for (int i = 1; i < n; i++) {
    uint64_t key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

static rx_mode_t parse_rx_mode(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--single-bit-diff") == 0) {
      return RX_MODE_SINGLE_BIT_DIFF;
    }
    if (strcmp(argv[i], "--single-bit") == 0) {
      return RX_MODE_SINGLE_BIT;
    }
  }
  return RX_MODE_BYTE;
}

int main(int argc, char **argv)
{
  rx_mode_t mode = parse_rx_mode(argc, argv);
  bool single_bit_mode = (mode != RX_MODE_BYTE);
  bool diff_mode = (mode == RX_MODE_SINGLE_BIT_DIFF);

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

  uint64_t samples[CALIBRATION_SAMPLES];
  uint64_t max_busy_cycles = 0;
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    uint64_t busy_cycles = sample_slot_cycles(buf);
    samples[i] = busy_cycles;
    if (busy_cycles > max_busy_cycles) {
      max_busy_cycles = busy_cycles;
    }
  }

  sort_u64(samples, CALIBRATION_SAMPLES);
  uint64_t baseline_cycles = samples[CALIBRATION_SAMPLES / 2];
  uint64_t p90_cycles = samples[(CALIBRATION_SAMPLES * 9) / 10];
  uint64_t spread_cycles = (p90_cycles > baseline_cycles) ? (p90_cycles - baseline_cycles) : 0;
  uint64_t margin_cycles = spread_cycles + MIN_MARGIN_CYCLES;
  if (margin_cycles < MIN_MARGIN_CYCLES) {
    margin_cycles = MIN_MARGIN_CYCLES;
  }
  if (margin_cycles > MAX_MARGIN_CYCLES) {
    margin_cycles = MAX_MARGIN_CYCLES;
  }

  uint64_t active_threshold_cycles = baseline_cycles + margin_cycles + NOISE_PAD_CYCLES;
  uint64_t diff_min_confidence = (spread_cycles * 2ULL) + 50000ULL;
  if (diff_min_confidence < DIFF_MIN_CONFIDENCE_FLOOR) {
    diff_min_confidence = DIFF_MIN_CONFIDENCE_FLOOR;
  }
  if (diff_min_confidence > DIFF_MIN_CONFIDENCE_CEIL) {
    diff_min_confidence = DIFF_MIN_CONFIDENCE_CEIL;
  }

  printf("Receiver now listening.\n");
  if (single_bit_mode) {
    if (diff_mode) {
      printf("Single-bit differential mode enabled.\n");
    } else {
      printf("Single-bit mode enabled.\n");
    }
  } else {
    printf("Byte mode enabled.\n");
  }
  printf("Busy baseline: %llu cycles, p90 idle: %llu cycles, active threshold: %llu cycles, max idle: %llu cycles\n",
         (unsigned long long)baseline_cycles,
         (unsigned long long)p90_cycles,
         (unsigned long long)active_threshold_cycles,
         (unsigned long long)max_busy_cycles);
  if (diff_mode) {
    printf("Differential confidence threshold: %llu cycles\n", (unsigned long long)diff_min_confidence);
  }

  enum {
    WAIT_PREAMBLE = 0,
    WAIT_GAP = 1,
    READ_BITS = 2
  } state = WAIT_PREAMBLE;

  while (keep_running) {
    if (state == WAIT_PREAMBLE) {
      int active_cnt = 0;
      for (int i = 0; i < PREAMBLE_SLOTS; i++) {
        if (sample_slot_active(buf, active_threshold_cycles)) {
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
        if (sample_slot_active(buf, active_threshold_cycles)) {
          gap_active++;
        }
      }

      if (diff_mode) {
        state = READ_BITS;
      } else if (gap_active <= GAP_MAX_ACTIVE) {
        state = READ_BITS;
      } else {
        state = WAIT_PREAMBLE;
      }
      continue;
    }

    if (state == READ_BITS) {
      if (single_bit_mode) {
        int bit;
        if (diff_mode) {
          int64_t score = 0;
          for (int r = 0; r < BIT_REPS; r++) {
            uint64_t first = sample_slot_cycles(buf);
            uint64_t second = sample_slot_cycles(buf);
            score += (int64_t)first - (int64_t)second;
          }
          if ((uint64_t)llabs(score) < diff_min_confidence) {
            state = WAIT_PREAMBLE;
            continue;
          }
          bit = (score > 0) ? 1 : 0;
        } else {
          int bit_active = 0;
          for (int r = 0; r < BIT_REPS; r++) {
            if (sample_slot_active(buf, active_threshold_cycles)) {
              bit_active++;
            }
          }
          bit = (bit_active >= ((BIT_REPS + 1) / 2)) ? 1 : 0;
        }
        printf("Received bit: %d\n", bit);
        fflush(stdout);
      } else {
        uint8_t value = 0;
        for (int b = 0; b < 8; b++) {
          int bit_active = 0;
          for (int r = 0; r < BIT_REPS; r++) {
            if (sample_slot_active(buf, active_threshold_cycles)) {
              bit_active++;
            }
          }
          int bit = (bit_active >= ((BIT_REPS + 1) / 2)) ? 1 : 0;
          value = (uint8_t)((value << 1) | bit);
        }
        printf("%u\n", (unsigned)value);
        fflush(stdout);
      }

      state = WAIT_PREAMBLE;
    }
  }

  printf("Receiver finished.\n");
  return 0;
}
