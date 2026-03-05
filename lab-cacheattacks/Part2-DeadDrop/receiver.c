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

#define PROBE_LINES 512

#define SLOT_CYCLES 5000000ULL
#define GUARD_CYCLES 200000ULL

#define PREAMBLE_BYTE 0xAA

#define BIT_REP 5
#define PREAMBLE_HITS 2

#define DETECT_DELTA 10
#define DETECT_RUN 6
#define QUIET_SLOTS 12
#define BUSY_SLOTS 12

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

  uint32_t x = 0x12345678u;
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

static uint32_t measure_slot(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t sum = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    char *p = buf + ((size_t)perm[k] * LINE_SIZE);
    sum += measure_one_block_access_time((ADDR_PTR)p);
  }
  return (uint32_t)(sum / nlines);
}

static uint32_t avg_over_n_slots(char *buf, const uint32_t *perm, uint32_t nlines, int n)
{
  uint64_t acc = 0;
  for (int i = 0; i < n; i++) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = next_slot_boundary(t);
    wait_until(slot_start + GUARD_CYCLES);
    acc += measure_slot(buf, perm, nlines);
    wait_until(slot_start + SLOT_CYCLES);
  }
  return (uint32_t)(acc / (uint64_t)n);
}

static void wait_for_busy_burst(char *buf, const uint32_t *perm, uint32_t nlines, uint32_t quiet)
{
  int run = 0;
  while (true) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = next_slot_boundary(t);

    wait_until(slot_start + GUARD_CYCLES);
    uint32_t avg = measure_slot(buf, perm, nlines);

    uint32_t diff = (avg > quiet) ? (avg - quiet) : (quiet - avg);
    if (diff >= DETECT_DELTA) {
      run++;
    } else {
      run = 0;
    }

    if (run >= DETECT_RUN) {
      return;
    }

    wait_until(slot_start + SLOT_CYCLES);
  }
}

static int recv_logical_bit(char *buf, const uint32_t *perm, uint32_t nlines, uint32_t thr)
{
  int ones = 0;
  for (int r = 0; r < BIT_REP; r++) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = next_slot_boundary(t);

    wait_until(slot_start + GUARD_CYCLES);
    uint32_t avg = measure_slot(buf, perm, nlines);
    ones += (avg > thr) ? 1 : 0;

    wait_until(slot_start + SLOT_CYCLES);
  }
  return (ones > (BIT_REP / 2)) ? 1 : 0;
}

static uint8_t recv_byte(char *buf, const uint32_t *perm, uint32_t nlines, uint32_t thr)
{
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    int bit = recv_logical_bit(buf, perm, nlines, thr);
    b = (uint8_t)((b << 1) | (bit & 1));
  }
  return b;
}

int main(void)
{
  char *buf = (char *)alloc_buffer();

  uint32_t nlines = PROBE_LINES;
  uint32_t *perm = (uint32_t *)malloc(nlines * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, nlines);

  printf("Please press enter.\n");
  char tmp[8];
  fgets(tmp, sizeof(tmp), stdin);

  printf("Receiver now listening.\n");

  fprintf(stderr, "Calibration: keep sender idle.\n");
  fflush(stderr);
  uint32_t quiet = avg_over_n_slots(buf, perm, nlines, QUIET_SLOTS);

  fprintf(stderr, "Calibration: in sender, type cal.\n");
  fflush(stderr);
  wait_for_busy_burst(buf, perm, nlines, quiet);

  uint32_t busy = avg_over_n_slots(buf, perm, nlines, BUSY_SLOTS);
  uint32_t thr = (quiet + busy) / 2;
  fprintf(stderr, "Calib quiet=%u busy=%u thr=%u polarity=high\n", quiet, busy, thr);
  fflush(stderr);

  int hits = 0;
  while (true) {
    uint8_t b = recv_byte(buf, perm, nlines, thr);

    if (hits < PREAMBLE_HITS) {
      if (b == PREAMBLE_BYTE) {
        hits++;
      } else {
        hits = 0;
      }
      continue;
    }

    uint8_t val = recv_byte(buf, perm, nlines, thr);
    printf("Received: %u\n", (unsigned)val);
    fflush(stdout);
    hits = 0;
  }
}
