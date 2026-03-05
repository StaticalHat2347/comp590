#include "util.h"
#include <sys/mman.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 21)
#define LINE_SIZE 64

#define THRASH_LINES 32768

#define SLOT_CYCLES 5000000ULL

#define PREAMBLE_BYTE 0xAA
#define BIT_REP 5

static inline uint64_t rdtsc64(void)
{
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

static inline void wait_until(uint64_t t)
{
  while (rdtsc64() < t) {
  }
}

static inline uint64_t next_slot_boundary(uint64_t now)
{
  return (now / SLOT_CYCLES + 1) * SLOT_CYCLES;
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
  *((volatile char *)buf) = 1;
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

static void thrash(char *buf, const uint32_t *perm, uint32_t nlines)
{
  for (uint32_t k = 0; k < nlines; k++) {
    volatile uint8_t *p = (volatile uint8_t *)(buf + ((size_t)perm[k] * LINE_SIZE));
    *p = (uint8_t)(*p + 1);
  }
}

static void send_physical_bit(char *buf, const uint32_t *perm, uint32_t nlines, int bit)
{
  uint64_t now = rdtsc64();
  uint64_t slot_start = next_slot_boundary(now);
  uint64_t slot_end = slot_start + SLOT_CYCLES;

  wait_until(slot_start);

  if (bit) {
    while (rdtsc64() < slot_end) {
      thrash(buf, perm, nlines);
    }
  } else {
    wait_until(slot_end);
  }
}

static void send_logical_bit(char *buf, const uint32_t *perm, uint32_t nlines, int bit)
{
  for (int r = 0; r < BIT_REP; r++) {
    send_physical_bit(buf, perm, nlines, bit);
  }
}

static void send_byte(char *buf, const uint32_t *perm, uint32_t nlines, uint8_t b)
{
  for (int i = 7; i >= 0; i--) {
    int bit = (b >> i) & 1;
    send_logical_bit(buf, perm, nlines, bit);
  }
}

static void send_calibration_burst(char *buf, const uint32_t *perm, uint32_t nlines)
{
  for (int i = 0; i < 40; i++) {
    send_physical_bit(buf, perm, nlines, 1);
  }
  for (int i = 0; i < 20; i++) {
    send_physical_bit(buf, perm, nlines, 0);
  }
}

static void send_frame(char *buf, const uint32_t *perm, uint32_t nlines, uint8_t val)
{
  for (int i = 0; i < 8; i++) {
    send_physical_bit(buf, perm, nlines, 0);
  }

  send_byte(buf, perm, nlines, PREAMBLE_BYTE);
  send_byte(buf, perm, nlines, PREAMBLE_BYTE);
  send_byte(buf, perm, nlines, val);

  for (int i = 0; i < 8; i++) {
    send_physical_bit(buf, perm, nlines, 0);
  }
}

int main(void)
{
  char *buf = (char *)alloc_buffer();

  uint32_t *perm = (uint32_t *)malloc(THRASH_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, THRASH_LINES);

  printf("Please type a number (0-255), or cal.\n");

  char line[128];
  while (fgets(line, sizeof(line), stdin)) {
    char *p = line;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
      p++;
    }
    if (*p == '[') {
      p++;
    }
    while (*p == ' ' || *p == '\t') {
      p++;
    }

    if (strncmp(p, "cal", 3) == 0) {
      send_calibration_burst(buf, perm, THRASH_LINES);
      continue;
    }

    int val = string_to_int(p);
    if (val < 0) {
      val = 0;
    }
    if (val > 255) {
      val = 255;
    }

    send_frame(buf, perm, THRASH_LINES, (uint8_t)val);
  }

  return 0;
}
