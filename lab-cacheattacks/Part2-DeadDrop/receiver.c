#include "util.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BUFF_SIZE     (1<<21)     // 2 MiB hugepage
#define LINE_SIZE     64          // typical cache line
#define PROBE_LINES   4096        // 4096 lines * 64B = 256 KiB probe footprint
#define SLOT_CYCLES   200000      // bit slot length (tune 100k-400k)
#define GUARD_CYCLES  20000       // skip boundary jitter at start of slot

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

  // trigger allocation + reduce first-touch noise
  *((volatile char*)buf) = 1;
  return buf;
}

// --- permutation builder to reduce prefetcher effects ---
static void build_perm(uint32_t *idx, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) idx[i] = i;

  uint32_t x = 0x12345678u; // deterministic seed
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

// Measure average access time (cycles) across many cache lines in this slot
static uint32_t measure_slot(char *buf, uint32_t *perm, uint32_t nlines) {
  uint64_t sum = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    char *p = buf + (perm[k] * LINE_SIZE);
    sum += measure_one_block_access_time((ADDR_PTR)p);
  }
  return (uint32_t)(sum / nlines);
}

int main(int argc, char **argv) {
  // Setup
  void *buf = alloc_hugepage();

  uint32_t nlines = PROBE_LINES;
  uint32_t *perm = (uint32_t*)malloc(nlines * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(EXIT_FAILURE);
  }
  build_perm(perm, nlines);

  printf("Please press enter.\n");

  char text_buf[2];
  fgets(text_buf, sizeof(text_buf), stdin);

  printf("Receiver now listening.\n");

  // --- Quick baseline calibration (sender should ideally be idle here) ---
  uint64_t now = rdtsc64();
  uint64_t slot0 = (now / SLOT_CYCLES + 1) * SLOT_CYCLES;

  uint32_t baseline = 0;
  for (int i = 0; i < 10; i++) {
    uint64_t start = slot0 + (uint64_t)i * SLOT_CYCLES;
    wait_until(start + GUARD_CYCLES);
    baseline += measure_slot((char*)buf, perm, nlines);
  }
  baseline /= 10;

  // Conservative threshold; tune if needed
  uint32_t threshold = baseline + 20;

  // Optional debug:
  // fprintf(stderr, "[receiver] baseline=%u threshold=%u\n", baseline, threshold);

  bool listening = true;
  while (listening) {
    uint64_t t = rdtsc64();
    uint64_t slot_start = (t / SLOT_CYCLES + 1) * SLOT_CYCLES;

    wait_until(slot_start + GUARD_CYCLES);
    uint32_t avg = measure_slot((char*)buf, perm, nlines);

    int bit = (avg > threshold) ? 1 : 0;

    // Print decoded bit
    printf("%d\n", bit);
    fflush(stdout);

    // Wait out the rest of the slot so we stay aligned
    wait_until(slot_start + SLOT_CYCLES);
  }

  printf("Receiver finished.\n");
  return 0;
}