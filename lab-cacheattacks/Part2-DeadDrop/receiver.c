#include "util.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFF_SIZE       (1<<21)
#define LINE_SIZE       64

#define PROBE_LINES     16384
#define SLOT_CYCLES     400000
#define GUARD_CYCLES    50000
#define THRESH_MARGIN   50

#define PREAMBLE_BYTE   0xAA  /* 10101010 */

static inline uint64_t rdtsc64(void) {
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

static inline void wait_until(uint64_t t) {
  while (rdtsc64() < t) { }
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

static inline uint64_t next_slot_boundary(uint64_t now) {
  return (now / SLOT_CYCLES + 1) * SLOT_CYCLES;
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
  char tmp[2];
  fgets(tmp, sizeof(tmp), stdin);

  printf("receiver now listening.\n");

  /* calibrate baseline with sender ideally idle */
  uint64_t now = rdtsc64();
  uint64_t slot0 = next_slot_boundary(now);

  uint32_t baseline = 0;
  for (int i = 0; i < 10; i++) {
    uint64_t start = slot0 + (uint64_t)i * SLOT_CYCLES;
    wait_until(start + GUARD_CYCLES);
    baseline += measure_slot((char*)buf, perm, nlines);
  }
  baseline /= 10;

  uint32_t threshold = baseline + THRESH_MARGIN;

  /* state machine: hunt preamble, then read one data byte */
  enum { HUNT_PREAMBLE = 0, READ_DATA = 1 } state = HUNT_PREAMBLE;

  uint8_t shift = 0;
  int shift_bits = 0;

  uint8_t data = 0;
  int data_bits = 0;

  while (true) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = next_slot_boundary(t);

    wait_until(slot_start + GUARD_CYCLES);
    uint32_t avg = measure_slot((char*)buf, perm, nlines);

    int bit = (avg > threshold) ? 1 : 0;

    /* adapt baseline only when we believe channel is quiet */
    if (bit == 0) {
      baseline = (baseline * 9 + avg) / 10;
      threshold = baseline + THRESH_MARGIN;
    }

    if (state == HUNT_PREAMBLE) {
      /* shift in bits until we see the preamble byte */
      shift = (uint8_t)((shift << 1) | (bit & 1));
      if (shift_bits < 8) shift_bits++;

      if (shift_bits == 8 && shift == PREAMBLE_BYTE) {
        state = READ_DATA;
        data = 0;
        data_bits = 0;
      }
    } else {
      /* read exactly 8 bits for the payload byte */
      data = (uint8_t)((data << 1) | (bit & 1));
      data_bits++;

      if (data_bits == 8) {
        printf("%u\n", (unsigned)data);
        fflush(stdout);

        /* reset to hunt the next message */
        state = HUNT_PREAMBLE;
        shift = 0;
        shift_bits = 0;
      }
    }

    wait_until(slot_start + SLOT_CYCLES);
  }

  return 0;
}