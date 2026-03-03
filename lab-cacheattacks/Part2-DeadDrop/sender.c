#include "util.h"
#include <sys/mman.h>
#include <errno.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 21)

#define CACHE_LINE_BYTES 64
#define SET_STRIDE_BYTES (1 << 16)
#define EVICTION_WAYS 24
#define WAY_STEP 7

#define NIBBLE_BASE_SET 8
#define START_SET 50
#define SEP_SET 51
#define END_SET 52

#define SYMBOL_ACTIVE_NS 120000000ULL
#define SYMBOL_GAP_NS 35000000ULL
#define FRAME_REPEAT_GAP_NS 90000000ULL
#define FRAME_REPETITIONS 2

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

static inline void hammer_set_once(void *buf, int set_idx)
{
  static volatile unsigned char sink = 0;
  for (int i = 0; i < EVICTION_WAYS; i++) {
    int way = (i * WAY_STEP) % EVICTION_WAYS;
    sink ^= *set_addr(buf, set_idx, way);
  }
}

static void tx_symbol(void *buf, int set_idx)
{
  uint64_t end_ns = monotonic_ns() + SYMBOL_ACTIVE_NS;
  while (monotonic_ns() < end_ns) {
    hammer_set_once(buf, set_idx);
  }
  sleep_ns(SYMBOL_GAP_NS);
}

static void tx_frame(void *buf, uint8_t value)
{
  int high = (value >> 4) & 0xF;
  int low = value & 0xF;
  int symbols[5] = {
    START_SET,
    NIBBLE_BASE_SET + high,
    SEP_SET,
    NIBBLE_BASE_SET + low,
    END_SET
  };

  for (int rep = 0; rep < FRAME_REPETITIONS; rep++) {
    for (int i = 0; i < 5; i++) {
      tx_symbol(buf, symbols[i]);
    }
    sleep_ns(FRAME_REPEAT_GAP_NS);
  }
}

static bool parse_uint8_line(char *line, uint8_t *value)
{
  char *endptr;
  errno = 0;
  long v = strtol(line, &endptr, 10);
  if (endptr == line || errno != 0) {
    return false;
  }

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
    endptr++;
  }
  if (*endptr != '\0') {
    return false;
  }
  if (v < 0 || v > 255) {
    return false;
  }

  *value = (uint8_t)v;
  return true;
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
    for (int way = 0; way < EVICTION_WAYS; way++) {
      *set_addr(buf, set_idx, way) = 1;
    }
  }

  printf("Please type an integer in [0,255] per line (or quit).\n");
  while (true) {
    char text_buf[128];
    if (fgets(text_buf, sizeof(text_buf), stdin) == NULL) {
      break;
    }
    if (strncmp(text_buf, "quit", 4) == 0 || strncmp(text_buf, "exit", 4) == 0) {
      break;
    }

    uint8_t value;
    if (!parse_uint8_line(text_buf, &value)) {
      printf("Invalid input. Enter an integer in [0,255].\n");
      continue;
    }

    tx_frame(buf, value);
  }

  printf("Sender finished.\n");
  return 0;
}
