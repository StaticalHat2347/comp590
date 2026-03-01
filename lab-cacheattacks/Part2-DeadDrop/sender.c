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
#define LOW_SET_COUNT 64
#define PAIR_OFFSET 64
#define SET_STRIDE_BYTES (1 << 16)
#define EVICTION_WAYS 16

#define SYMBOL_NS 300000000ULL
#define GAP_NS 120000000ULL

#define BIT0_MARKER 5
#define BIT1_MARKER 59
#define PREAMBLE 0xA5

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

static inline void hammer_pair_once(void *buf, int low_set_idx)
{
  static volatile unsigned char sink = 0;
  int set_a = low_set_idx;
  int set_b = low_set_idx + PAIR_OFFSET;

  for (int way = 0; way < EVICTION_WAYS; way++) {
    sink ^= *set_addr(buf, set_a, way);
    sink ^= *set_addr(buf, set_b, way);
  }
}

static void transmit_marker_symbol(void *buf, int low_set_idx)
{
  uint64_t end_time = monotonic_ns() + SYMBOL_NS;
  while (monotonic_ns() < end_time) {
    hammer_pair_once(buf, low_set_idx);
  }

  struct timespec gap;
  gap.tv_sec = 0;
  gap.tv_nsec = GAP_NS;
  nanosleep(&gap, NULL);
}

static void transmit_bit(void *buf, int bit)
{
  transmit_marker_symbol(buf, bit ? BIT1_MARKER : BIT0_MARKER);
}

static void transmit_byte(void *buf, uint8_t value)
{
  for (int b = 7; b >= 0; b--) {
    transmit_bit(buf, (PREAMBLE >> b) & 1);
  }
  for (int b = 7; b >= 0; b--) {
    transmit_bit(buf, (value >> b) & 1);
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
  void *buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB,
                   -1, 0);
  if (buf == (void *)-1) {
    perror("mmap() error\n");
    exit(EXIT_FAILURE);
  }

  for (int set_idx = 0; set_idx < (LOW_SET_COUNT + PAIR_OFFSET); set_idx++) {
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

    transmit_byte(buf, value);
  }

  printf("Sender finished.\n");
  return 0;
}
