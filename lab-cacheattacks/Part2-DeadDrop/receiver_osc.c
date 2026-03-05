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
#define DELAY_ITERS 120000000ULL

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
    exit(1);
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

static uint32_t measure_avg(char *buf, const uint32_t *perm, uint32_t nlines)
{
  uint64_t sum = 0;
  for (uint32_t k = 0; k < nlines; k++) {
    char *p = buf + ((size_t)perm[k] * LINE_SIZE);
    sum += measure_one_block_access_time((ADDR_PTR)p);
  }
  return (uint32_t)(sum / nlines);
}

int main(void)
{
  char *buf = (char *)alloc_buffer();
  uint32_t *perm = (uint32_t *)malloc(PROBE_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(1);
  }
  build_perm(perm, PROBE_LINES);

  printf("osc receiver: printing avg cycles.\n");
  fflush(stdout);

  while (1) {
    uint32_t avg = measure_avg(buf, perm, PROBE_LINES);
    printf("%u\n", avg);
    fflush(stdout);

    for (volatile uint64_t i = 0; i < DELAY_ITERS; i++) {
    }
  }
}
