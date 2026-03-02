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
#define EVICTION_WAYS 32

#define MARKER_SET 27

#define SLOT_NS 120000000ULL
#define SYNC_ACTIVE_SLOTS 6
#define SYNC_GAP_SLOTS 2
#define PRE_DATA_GUARD_SLOTS 1
#define BYTE_GAP_SLOTS 4

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

static inline void hammer_set_once(void *buf, int set_idx)
{
  static volatile unsigned char sink = 0;
  for (int way = 0; way < EVICTION_WAYS; way++) {
    sink ^= *set_addr(buf, set_idx, way);
  }
}

static void tx_active_slot(void *buf)
{
  uint64_t end = monotonic_ns() + SLOT_NS;
  while (monotonic_ns() < end) {
    hammer_set_once(buf, MARKER_SET);
  }
}

static void tx_idle_slots(int slots)
{
  struct timespec ts;
  uint64_t total = SLOT_NS * (uint64_t)slots;
  ts.tv_sec = (time_t)(total / 1000000000ULL);
  ts.tv_nsec = (long)(total % 1000000000ULL);
  nanosleep(&ts, NULL);
}

static void tx_byte(void *buf, uint8_t value)
{
  for (int i = 0; i < SYNC_ACTIVE_SLOTS; i++) {
    tx_active_slot(buf);
  }
  tx_idle_slots(SYNC_GAP_SLOTS);
  tx_idle_slots(PRE_DATA_GUARD_SLOTS);

  for (int b = 7; b >= 0; b--) {
    int bit = (value >> b) & 1;
    if (bit) {
      tx_active_slot(buf);
    } else {
      tx_idle_slots(1);
    }
  }

  tx_idle_slots(BYTE_GAP_SLOTS);
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

    tx_byte(buf, value);
  }

  printf("Sender finished.\n");
  return 0;
}
