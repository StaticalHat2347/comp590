#include "util.h"
#include <errno.h>
#include <sys/mman.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 21)
#define LINE_SIZE 64
#define ACTIVE_LINES 4096

#define SLOT_NS 45000000ULL
#define TRAIL_IDLE_SLOTS 4
#define TX_REPEATS 3

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

  uint32_t x = 0xA5A5A5A5u;
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

static void send_active_slot(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t end_ns = monotonic_ns() + SLOT_NS;
  while (monotonic_ns() < end_ns) {
    thrash_once(buf, perm, nlines);
  }
}

static void send_idle_slot(void)
{
  sleep_ns(SLOT_NS);
}

static void send_symbol(char *buf, const uint32_t *perm, uint32_t nlines, int active)
{
  if (active) {
    send_active_slot(buf, perm, nlines);
  } else {
    send_idle_slot();
  }
}

static void send_sync(char *buf, const uint32_t *perm, uint32_t nlines)
{
  /* Sync word: 1 1 0 0 1 1 */
  send_symbol(buf, perm, nlines, 1);
  send_symbol(buf, perm, nlines, 1);
  send_symbol(buf, perm, nlines, 0);
  send_symbol(buf, perm, nlines, 0);
  send_symbol(buf, perm, nlines, 1);
  send_symbol(buf, perm, nlines, 1);
}

static void send_bit(char *buf, const uint32_t *perm, uint32_t nlines, int bit)
{
  /* Manchester-like: 1 => active,idle ; 0 => idle,active */
  if (bit) {
    send_symbol(buf, perm, nlines, 1);
    send_symbol(buf, perm, nlines, 0);
  } else {
    send_symbol(buf, perm, nlines, 0);
    send_symbol(buf, perm, nlines, 1);
  }
}

static void send_u8(char *buf, const uint32_t *perm, uint32_t nlines, uint8_t value)
{
  for (int rep = 0; rep < TX_REPEATS; rep++) {
    send_sync(buf, perm, nlines);
    for (int bit = 7; bit >= 0; bit--) {
      send_bit(buf, perm, nlines, (value >> bit) & 1);
    }
    for (int i = 0; i < TRAIL_IDLE_SLOTS; i++) {
      send_idle_slot();
    }
  }
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

  uint32_t *perm = (uint32_t *)malloc(ACTIVE_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, ACTIVE_LINES);

  printf("Please type an integer in [0,255] per line (or quit).\n");
  fflush(stdout);

  char line[128];
  while (fgets(line, sizeof(line), stdin)) {
    if (strncmp(line, "quit", 4) == 0 || strncmp(line, "exit", 4) == 0) {
      break;
    }

    uint8_t value = 0;
    if (!parse_u8_line(line, &value)) {
      printf("Invalid input. Enter an integer in [0,255].\n");
      fflush(stdout);
      continue;
    }

    send_u8(buf, perm, ACTIVE_LINES, value);
    printf("Please type an integer in [0,255] per line (or quit).\n");
    fflush(stdout);
  }

  return 0;
}
