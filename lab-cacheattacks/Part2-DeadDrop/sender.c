#include "util.h"
#include <sys/mman.h>
#include <errno.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 23)
#define LINE_SIZE 64
#define THRASH_LINES (BUFF_SIZE / LINE_SIZE)

#define SLOT_NS 60000000ULL
#define PREAMBLE_SLOTS 10
#define GAP_SLOTS 2
#define BIT_REP 5
#define TRAIL_SLOTS 3

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

  uint32_t x = 0xCAFEBABEu;
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

static inline void thrash_once(char *buf, const uint32_t *perm, uint32_t nlines)
{
  static volatile uint8_t sink = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    volatile uint8_t *p = (volatile uint8_t *)(buf + ((size_t)perm[k] * LINE_SIZE));
    sink ^= *p;
  }
}

static void tx_active_slot(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t end_ns = monotonic_ns() + SLOT_NS;
  while (monotonic_ns() < end_ns) {
    thrash_once(buf, perm, nlines);
  }
}

static void tx_idle_slots(int nslots)
{
  sleep_ns(SLOT_NS * (uint64_t)nslots);
}

static void tx_logical_bit(char *buf, const uint32_t *perm, uint32_t nlines, int bit)
{
  for (int r = 0; r < BIT_REP; r++) {
    if (bit) {
      tx_active_slot(buf, perm, nlines);
    } else {
      tx_idle_slots(1);
    }
  }
}

static void tx_frame(char *buf, const uint32_t *perm, uint32_t nlines, uint8_t value)
{
  for (int i = 0; i < PREAMBLE_SLOTS; i++) {
    tx_active_slot(buf, perm, nlines);
  }

  tx_idle_slots(GAP_SLOTS);

  for (int b = 7; b >= 0; b--) {
    int bit = (value >> b) & 1;
    tx_logical_bit(buf, perm, nlines, bit);
  }

  tx_idle_slots(TRAIL_SLOTS);
}

static bool parse_u8_line(const char *line, uint8_t *value)
{
  char *endptr;
  errno = 0;
  long v = strtol(line, &endptr, 10);
  if (endptr == line || errno != 0) {
    return false;
  }

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\n' || *endptr == '\r') {
    endptr++;
  }
  if (*endptr != '\0' || v < 0 || v > 255) {
    return false;
  }

  *value = (uint8_t)v;
  return true;
}

int main(void)
{
  char *buf = (char *)alloc_buffer();

  for (int i = 0; i < BUFF_SIZE; i += LINE_SIZE) {
    buf[i] = (char)(i & 0xFF);
  }

  uint32_t *perm = (uint32_t *)malloc(THRASH_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, THRASH_LINES);

  printf("Please type an integer in [0,255] per line (or quit).\n");

  char line[128];
  while (fgets(line, sizeof(line), stdin)) {
    if (strncmp(line, "quit", 4) == 0 || strncmp(line, "exit", 4) == 0) {
      break;
    }

    uint8_t value;
    if (!parse_u8_line(line, &value)) {
      printf("Invalid input. Enter an integer in [0,255].\n");
      continue;
    }

    tx_frame(buf, perm, THRASH_LINES, value);
  }

  return 0;
}
