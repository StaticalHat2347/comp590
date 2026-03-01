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
#define LOW_SET_COUNT 64
#define PAIR_OFFSET 64
#define SET_STRIDE_BYTES (1 << 14)
#define PROBE_WAYS 64

#define CALIBRATION_ROUNDS 30
#define BUSY_MARGIN 20
#define GAP_MARGIN 10

#define SEGMENT_IDLE_SCANS 5
#define SEGMENT_MIN_VOTES 8
#define MIN_DOMINANCE_PCT 70

#define BIT0_MARKER 5
#define BIT1_MARKER 59
#define PREAMBLE 0xA5

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

static CYCLES measure_pair_latency(void *buf, int low_set_idx)
{
  uint64_t sum_a = 0;
  uint64_t sum_b = 0;
  int set_a = low_set_idx;
  int set_b = low_set_idx + PAIR_OFFSET;

  for (int way = 0; way < PROBE_WAYS; way++) {
    sum_a += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_a, way));
    sum_b += measure_one_block_access_time((ADDR_PTR)set_addr(buf, set_b, way));
  }

  CYCLES avg_a = (CYCLES)(sum_a / PROBE_WAYS);
  CYCLES avg_b = (CYCLES)(sum_b / PROBE_WAYS);
  return (avg_a > avg_b) ? avg_a : avg_b;
}

static void detect_best_pair(void *buf, int *best_idx, CYCLES *best, CYCLES *second)
{
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
  uint64_t best_sum = 0;
  uint64_t gap_sum = 0;
  for (int r = 0; r < CALIBRATION_ROUNDS; r++) {
    int best_idx;
    CYCLES best, second;
    detect_best_pair(buf, &best_idx, &best, &second);
    (void)best_idx;
    best_sum += best;
    gap_sum += (best > second) ? (best - second) : 0;
  }

  CYCLES avg_best = (CYCLES)(best_sum / CALIBRATION_ROUNDS);
  CYCLES avg_gap = (CYCLES)(gap_sum / CALIBRATION_ROUNDS);
  *busy_threshold = (CYCLES)(avg_best + BUSY_MARGIN);
  *gap_threshold = (CYCLES)(avg_gap + GAP_MARGIN);
}

static int classify_bit_from_symbol(int symbol_idx)
{
  if (symbol_idx == BIT0_MARKER) return 0;
  if (symbol_idx == BIT1_MARKER) return 1;
  return -1;
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

  for (int set_idx = 0; set_idx < (LOW_SET_COUNT + PAIR_OFFSET); set_idx++) {
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

  int segment_votes[LOW_SET_COUNT] = {0};
  bool in_segment = false;
  int idle_scans = 0;
  int total_votes = 0;
  int prev_hot_symbol = -1;
  int hot_run_len = 0;

  uint8_t stream_reg = 0;
  int payload_bits_left = 0;
  uint8_t payload = 0;

  while (keep_running) {
    int best_idx;
    CYCLES best, second;
    detect_best_pair(buf, &best_idx, &best, &second);

    bool hot = (best_idx >= 0) && (best >= busy_threshold) && ((best - second) >= gap_threshold);
    if (hot) {
      if (best_idx == prev_hot_symbol) {
        hot_run_len++;
      } else {
        prev_hot_symbol = best_idx;
        hot_run_len = 1;
      }

      if (!in_segment) {
        memset(segment_votes, 0, sizeof(segment_votes));
        total_votes = 0;
        idle_scans = 0;
        in_segment = true;
      }
      // Count only stable (consecutive) hot detections to filter noise spikes.
      if (hot_run_len >= 2) {
        segment_votes[best_idx]++;
        total_votes++;
      }
      idle_scans = 0;
      continue;
    }

    prev_hot_symbol = -1;
    hot_run_len = 0;

    if (!in_segment) {
      continue;
    }

    idle_scans++;
    if (idle_scans < SEGMENT_IDLE_SCANS) {
      continue;
    }

    int symbol = 0;
    for (int i = 1; i < LOW_SET_COUNT; i++) {
      if (segment_votes[i] > segment_votes[symbol]) {
        symbol = i;
      }
    }

    if (total_votes >= SEGMENT_MIN_VOTES &&
        (segment_votes[symbol] * 100) >= (MIN_DOMINANCE_PCT * total_votes)) {
      int bit = classify_bit_from_symbol(symbol);
      if (bit >= 0) {
        if (payload_bits_left > 0) {
          payload = (uint8_t)((payload << 1) | (uint8_t)bit);
          payload_bits_left--;
          if (payload_bits_left == 0) {
            printf("Received: %u\n", (unsigned)payload);
            fflush(stdout);
            payload = 0;
          }
        } else {
          stream_reg = (uint8_t)((stream_reg << 1) | (uint8_t)bit);
          if (stream_reg == PREAMBLE) {
            payload = 0;
            payload_bits_left = 8;
          }
        }
      }
    }

    in_segment = false;
    idle_scans = 0;
    total_votes = 0;
  }

  printf("Receiver finished.\n");
  return 0;
}
