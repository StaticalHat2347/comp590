#include "util.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFF_SIZE       (1<<21)
#define LINE_SIZE       64

#define THRASH_LINES    32768
#define SLOT_CYCLES     400000

#define PREAMBLE_BYTE   0xAA  /* 10101010 */

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

  uint32_t x = 0xCAFEBABEu;
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

static void thrash(char *buf, uint32_t *perm, uint32_t nlines) {
  for (uint32_t k = 0; k < nlines; k++) {
    volatile uint8_t *p = (volatile uint8_t*)(buf + perm[k] * LINE_SIZE);
    *p = (uint8_t)(*p + 1);
  }
}

static void send_bit(char *buf, uint32_t *perm, uint32_t nlines, int bit) {
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

static void send_byte(char *buf, uint32_t *perm, uint32_t nlines, uint8_t b) {
  for (int i = 7; i >= 0; i--) {
    int bit = (b >> i) & 1;
    send_bit(buf, perm, nlines, bit);
  }
}

int main(int argc, char **argv) {
  void *buf = alloc_hugepage();

  uint32_t nlines = THRASH_LINES;
  uint32_t *perm = (uint32_t*)malloc(nlines * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, nlines);

  printf("please type a number (0-255).\n");

  while (true) {
    char text_buf[128];
    if (!fgets(text_buf, sizeof(text_buf), stdin)) break;

    int val = string_to_int(text_buf);
    if (val < 0) val = 0;
    if (val > 255) val = 255;

    /* send a short quiet gap to help receiver baseline stay stable */
    for (int i = 0; i < 4; i++) {
      send_bit((char*)buf, perm, nlines, 0);
    }

    /* transmit frame: preamble then payload byte */
    send_byte((char*)buf, perm, nlines, (uint8_t)PREAMBLE_BYTE);
    send_byte((char*)buf, perm, nlines, (uint8_t)val);

    /* trailing gap */
    for (int i = 0; i < 4; i++) {
      send_bit((char*)buf, perm, nlines, 0);
    }
  }

  return 0;
}