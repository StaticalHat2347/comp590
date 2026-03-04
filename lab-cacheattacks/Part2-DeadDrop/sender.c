#include "util.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFF_SIZE      (1<<21)     // 2 MiB hugepage
#define LINE_SIZE      64
#define THRASH_LINES   16384       // 16384 * 64B = 1 MiB working set
#define SLOT_CYCLES    200000      // must match receiver
#define GUARD_CYCLES   20000       // not strictly needed here, but keep consistent

// --- TSC helpers ---
static inline uint64_t rdtsc64(void) {
  uint32_t lo, hi;
  asm volatile("lfence\n\trdtsc\n\t" : "=a"(lo), "=d"(hi) :: "memory");
  return ((uint64_t)hi << 32) | lo;
}

static inline void wait_until(uint64_t t) {
  while (rdtsc64() < t) { /* spin */ }
}

// --- hugepage alloc ---
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

// --- permutation builder ---
static void build_perm(uint32_t *idx, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) idx[i] = i;

  uint32_t x = 0xCAFEBABEu; // deterministic seed
  for (uint32_t i = n - 1; i > 0; i--) {
    // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    uint32_t j = x % (i + 1);
    uint32_t tmp = idx[i];
    idx[i] = idx[j];
    idx[j] = tmp;
  }
}

// Read many distinct cache lines to create L2 contention
static void thrash(char *buf, uint32_t *perm, uint32_t nlines) {
  volatile uint64_t sink = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    sink += *(volatile uint8_t*)(buf + perm[k] * LINE_SIZE);
  }
  (void)sink;
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

  printf("Please type a message.\n");

  bool sending = true;
  while (sending) {
    char text_buf[128];
    if (!fgets(text_buf, sizeof(text_buf), stdin)) break;

    // For Part 2.2, easiest is: type "0" or "1"
    // Anything non-zero -> send 1
    int bit = string_to_int(text_buf) ? 1 : 0;

    uint64_t now = rdtsc64();
    uint64_t slot_start = (now / SLOT_CYCLES + 1) * SLOT_CYCLES;
    uint64_t slot_end   = slot_start + SLOT_CYCLES;

    // align to slot boundary
    wait_until(slot_start);

    if (bit == 1) {
      // thrash continuously until slot ends
      while (rdtsc64() < slot_end) {
        thrash((char*)buf, perm, nlines);
      }
    } else {
      // idle until slot ends
      wait_until(slot_end);
    }
  }

  printf("Sender finished.\n");
  return 0;
}