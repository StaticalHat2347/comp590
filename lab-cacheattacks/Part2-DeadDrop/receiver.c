#include "util.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFF_SIZE       (1<<21)
#define LINE_SIZE       64

#define PROBE_LINES     1024

#define SLOT_CYCLES     5000000ULL
#define GUARD_CYCLES    200000ULL

#define PREAMBLE_BYTE   0xAA  /* 10101010 */
#define BIT_REP         3

static inline uint64_t rdtsc64(void) {
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

static inline void wait_until(uint64_t t) {
  while (rdtsc64() < t) { }
}

static inline uint64_t next_slot_boundary(uint64_t now) {
  return (now / SLOT_CYCLES + 1) * SLOT_CYCLES;
}

static void *alloc_hugepage(void) {
  void *buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB,
                   -1, 0);
  if (buf == (void*)-1) {
    perror("mmap");
    exit(EXIT_FAILURE);
  }
  *((volatile char*)buf) = 1;
  return buf;
}

static void build_perm(uint32_t *idx, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) idx[i] = i;

  uint32_t x = 0x12345678u;
  for (uint32_t i = n - 1; i > 0; i--) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    uint32_t j = x % (i + 1);
    uint32_t tmp = idx[i];
    idx[i] = idx[j];
    idx[j] = tmp;
  }
}

static uint32_t measure_slot(char *buf, uint32_t *perm, uint32_t nlines) {
  uint64_t sum = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    char *p = buf + (perm[k] * LINE_SIZE);
    sum += measure_one_block_access_time((ADDR_PTR)p);
  }
  return (uint32_t)(sum / nlines);
}

static uint32_t avg_over_n_slots(char *buf, uint32_t *perm, uint32_t nlines, int n) {
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

static int decode_physical_bit(uint32_t avg, uint32_t thr, bool one_is_high) {
  if (one_is_high) return (avg > thr) ? 1 : 0;
  return (avg < thr) ? 1 : 0;
}

static int decode_logical_bit(char *buf, uint32_t *perm, uint32_t nlines,
                              uint32_t thr, bool one_is_high) {
  int ones = 0;
  for (int r = 0; r < BIT_REP; r++) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = next_slot_boundary(t);
    wait_until(slot_start + GUARD_CYCLES);
    uint32_t avg = measure_slot(buf, perm, nlines);
    ones += decode_physical_bit(avg, thr, one_is_high);
    wait_until(slot_start + SLOT_CYCLES);
  }
  return (ones > (BIT_REP / 2)) ? 1 : 0;
}

static uint8_t recv_byte(char *buf, uint32_t *perm, uint32_t nlines,
                         uint32_t thr, bool one_is_high) {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    int bit = decode_logical_bit(buf, perm, nlines, thr, one_is_high);
    b = (uint8_t)((b << 1) | (bit & 1));
  }
  return b;
}

int main(int argc, char **argv) {
  void *buf = alloc_hugepage();

  uint32_t nlines = PROBE_LINES;
  uint32_t *perm = (uint32_t*)malloc(nlines * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, nlines);

  printf("please press enter.\n");
  char line[32];
  fgets(line, sizeof(line), stdin);

  printf("receiver now listening.\n");

  /* quiet calibration */
  fprintf(stderr, "calibration: keep sender idle.\n");
  fflush(stderr);
  uint32_t quiet = avg_over_n_slots((char*)buf, perm, nlines, 12);

  /* handshaked busy calibration */
  fprintf(stderr, "calibration: start sender, type cal, then press enter here.\n");
  fflush(stderr);
  fgets(line, sizeof(line), stdin);

  uint32_t busy = avg_over_n_slots((char*)buf, perm, nlines, 12);

  uint32_t thr = (quiet + busy) / 2;
  bool one_is_high = (busy > quiet);

  fprintf(stderr, "calib quiet=%u busy=%u thr=%u polarity=%s\n",
          quiet, busy, thr, one_is_high ? "high" : "low");
  fflush(stderr);

  /* require two consecutive preamble bytes to lock */
  int preamble_hits = 0;

  while (true) {
    uint8_t b = recv_byte((char*)buf, perm, nlines, thr, one_is_high);

    if (preamble_hits < 2) {
      if (b == PREAMBLE_BYTE) preamble_hits++;
      else preamble_hits = 0;
      continue;
    }

    uint8_t val = recv_byte((char*)buf, perm, nlines, thr, one_is_high);
    printf("%u\n", (unsigned)val);
    fflush(stdout);

    preamble_hits = 0;
  }

  return 0;
}