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
#define PROBE_WAYS 24
#define WAY_STEP 7

#define NIBBLE_BASE_SET 8
#define START_SET 50
#define SEP_SET 51
#define END_SET 52

#define PROBE_DELAY_NS 600000ULL

#define CALIBRATION_SAMPLES 140
#define DIFF_MARGIN 5
#define MIN_DIFF_THRESHOLD 3
#define DOMINANCE_MARGIN 2

#define MIN_SEGMENT_RUN 4
#define DEDUPE_WINDOW_NS 1400000000ULL

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

static void prime_set(void *buf, int set_idx)
{
  static volatile unsigned char sink = 0;
  for (int i = 0; i < PROBE_WAYS; i++) {
    int way = (i * WAY_STEP) % PROBE_WAYS;
    sink ^= *set_addr(buf, set_idx, way);
  }
}

static CYCLES probe_set_latency(void *buf, int set_idx)
{
  uint64_t sum = 0;
  for (int i = 0; i < PROBE_WAYS; i++) {
    int way = (i * WAY_STEP) % PROBE_WAYS;
    sum += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_idx, way));
  }
  return (CYCLES)(sum / PROBE_WAYS);
}

static int symbol_to_nibble(int symbol)
{
  if (symbol < NIBBLE_BASE_SET || symbol > NIBBLE_BASE_SET + 15) {
    return -1;
  }
  return symbol - NIBBLE_BASE_SET;
}

static int sample_best_symbol(void *buf, CYCLES *best_latency, CYCLES *best_diff)
{
  static const int symbols[] = {
    START_SET, SEP_SET, END_SET,
    NIBBLE_BASE_SET + 0,  NIBBLE_BASE_SET + 1,  NIBBLE_BASE_SET + 2,  NIBBLE_BASE_SET + 3,
    NIBBLE_BASE_SET + 4,  NIBBLE_BASE_SET + 5,  NIBBLE_BASE_SET + 6,  NIBBLE_BASE_SET + 7,
    NIBBLE_BASE_SET + 8,  NIBBLE_BASE_SET + 9,  NIBBLE_BASE_SET + 10, NIBBLE_BASE_SET + 11,
    NIBBLE_BASE_SET + 12, NIBBLE_BASE_SET + 13, NIBBLE_BASE_SET + 14, NIBBLE_BASE_SET + 15
  };
  enum { SYMBOL_COUNT = (int)(sizeof(symbols) / sizeof(symbols[0])) };

  uint64_t sum = 0;
  CYCLES max_lat = 0;
  CYCLES second_lat = 0;
  int max_idx = 0;
  static int probe_phase = 0;

  for (int i = 0; i < SYMBOL_COUNT; i++) {
    int idx = (i + probe_phase) % SYMBOL_COUNT;
    prime_set(buf, symbols[idx]);
  }

  sleep_ns(PROBE_DELAY_NS);

  for (int i = 0; i < SYMBOL_COUNT; i++) {
    int idx = (i + probe_phase) % SYMBOL_COUNT;
    CYCLES lat = probe_set_latency(buf, symbols[idx]);
    sum += lat;

    if (lat > max_lat) {
      second_lat = max_lat;
      max_lat = lat;
      max_idx = idx;
    } else if (lat > second_lat) {
      second_lat = lat;
    }
  }

  probe_phase = (probe_phase + 5) % SYMBOL_COUNT;

  CYCLES mean = (CYCLES)(sum / SYMBOL_COUNT);
  CYCLES diff = (max_lat > mean) ? (max_lat - mean) : 0;
  CYCLES dominance = (max_lat > second_lat) ? (max_lat - second_lat) : 0;

  *best_latency = max_lat;
  *best_diff = diff;

  if (dominance < DOMINANCE_MARGIN) {
    return -1;
  }
  return symbols[max_idx];
}

int main(int argc, char **argv)
{
  int mmap_flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB;
  void *buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
  if (buf == (void *)-1 && MAP_HUGETLB != 0) {
    mmap_flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE;
    buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
  }
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

  uint64_t diff_sum = 0;
  uint64_t lat_sum = 0;
  for (int i = 0; i < CALIBRATION_SAMPLES; i++) {
    CYCLES max_lat = 0;
    CYCLES max_diff = 0;
    (void)sample_best_symbol(buf, &max_lat, &max_diff);
    lat_sum += max_lat;
    diff_sum += max_diff;
    sleep_ns(2000000ULL);
  }

  CYCLES latency_baseline = (CYCLES)(lat_sum / CALIBRATION_SAMPLES);
  CYCLES diff_baseline = (CYCLES)(diff_sum / CALIBRATION_SAMPLES);
  CYCLES active_diff_threshold = (CYCLES)(diff_baseline + DIFF_MARGIN);
  if (active_diff_threshold < MIN_DIFF_THRESHOLD) {
    active_diff_threshold = MIN_DIFF_THRESHOLD;
  }

  printf("Receiver now listening.\n");
  printf("Latency baseline: %u, active threshold: %u\n",
         latency_baseline, (unsigned)(latency_baseline + active_diff_threshold));

  enum {
    WAIT_START = 0,
    WAIT_HIGH = 1,
    WAIT_SEP = 2,
    WAIT_LOW = 3,
    WAIT_END = 4
  } decode_state = WAIT_START;

  int current_symbol = -1;
  int current_run = 0;
  int high_nibble = 0;
  int low_nibble = 0;

  uint8_t last_value = 0;
  uint64_t last_print_ns = 0;

  while (keep_running) {
    CYCLES max_lat = 0;
    CYCLES max_diff = 0;
    int symbol = sample_best_symbol(buf, &max_lat, &max_diff);

    if (max_diff < active_diff_threshold) {
      symbol = -1;
    }

    if (symbol == current_symbol) {
      current_run++;
      continue;
    }

    if (current_symbol != -1 && current_run >= MIN_SEGMENT_RUN) {
      if (decode_state == WAIT_START) {
        if (current_symbol == START_SET) {
          decode_state = WAIT_HIGH;
        }
      } else if (decode_state == WAIT_HIGH) {
        int nib = symbol_to_nibble(current_symbol);
        if (nib >= 0) {
          high_nibble = nib;
          decode_state = WAIT_SEP;
        } else if (current_symbol == START_SET) {
          decode_state = WAIT_HIGH;
        } else {
          decode_state = WAIT_START;
        }
      } else if (decode_state == WAIT_SEP) {
        if (current_symbol == SEP_SET) {
          decode_state = WAIT_LOW;
        } else if (current_symbol == START_SET) {
          decode_state = WAIT_HIGH;
        } else {
          decode_state = WAIT_START;
        }
      } else if (decode_state == WAIT_LOW) {
        int nib = symbol_to_nibble(current_symbol);
        if (nib >= 0) {
          low_nibble = nib;
          decode_state = WAIT_END;
        } else if (current_symbol == START_SET) {
          decode_state = WAIT_HIGH;
        } else {
          decode_state = WAIT_START;
        }
      } else if (decode_state == WAIT_END) {
        if (current_symbol == END_SET) {
          uint8_t value = (uint8_t)((high_nibble << 4) | low_nibble);
          uint64_t now_ns = monotonic_ns();
          if (!(value == last_value && (now_ns - last_print_ns) < DEDUPE_WINDOW_NS)) {
            printf("Received: %u\n", (unsigned)value);
            fflush(stdout);
            last_value = value;
            last_print_ns = now_ns;
          }
          decode_state = WAIT_START;
        } else if (current_symbol == START_SET) {
          decode_state = WAIT_HIGH;
        } else {
          decode_state = WAIT_START;
        }
      }
    }

    current_symbol = symbol;
    current_run = 1;
  }

  printf("Receiver finished.\n");
  return 0;
}
