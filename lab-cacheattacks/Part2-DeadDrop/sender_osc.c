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
#define OFF_ITERS 1500000000ULL
#define ON_ITERS 1500000000ULL

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

int main(void)
{
  char *buf = (char *)alloc_buffer();
  uint32_t *perm = (uint32_t *)malloc(THRASH_LINES * sizeof(uint32_t));
  if (!perm) {
    perror("malloc");
    exit(1);
  }
  build_perm(perm, THRASH_LINES);

  printf("osc sender: toggling on/off every second.\n");
  fflush(stdout);

  while (1) {
    for (volatile uint64_t i = 0; i < OFF_ITERS; i++) {
    }

    for (volatile uint64_t i = 0; i < ON_ITERS; i++) {
      thrash(buf, perm, THRASH_LINES);
    }
  }
}
