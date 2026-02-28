
#include"util.h"
// mman library to be used for hugepage allocations (e.g. mmap or posix_memalign only)
#include <sys/mman.h>
#include <errno.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1<<21)

#define CACHE_LINE_BYTES 64
#define NUM_SYMBOL_SETS 256
#define SET_STRIDE_BYTES (1 << 16)   // 64 KB keeps the same L2 set index
#define EVICTION_WAYS 16

#define SYMBOL_NS 120000000ULL        // 120 ms per symbol
#define GAP_NS 30000000ULL            // 30 ms of silence between symbols
#define FRAME_HEADER_SET 255

static inline uint64_t monotonic_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline volatile unsigned char *set_addr(void *buf, int set_idx, int way)
{
  return (volatile unsigned char *)((unsigned char *)buf +
         (set_idx * CACHE_LINE_BYTES) + (way * SET_STRIDE_BYTES));
}

static inline void hammer_set_once(void *buf, uint8_t set_idx)
{
  for (int way = 0; way < EVICTION_WAYS; way++) {
    (*set_addr(buf, set_idx, way))++;
  }
}

static void transmit_symbol(void *buf, uint8_t symbol)
{
  uint64_t end_time = monotonic_ns() + SYMBOL_NS;
  while (monotonic_ns() < end_time) {
    hammer_set_once(buf, symbol);
  }

  struct timespec gap;
  gap.tv_sec = 0;
  gap.tv_nsec = GAP_NS;
  nanosleep(&gap, NULL);
}

static bool parse_uint8_line(char *line, uint8_t *value)
{
  char *endptr;
  errno = 0;
  long v = strtol(line, &endptr, 10);

  if (endptr == line || errno != 0) {
    return false;
  }

  while (*endptr == ' ' || *endptr == '\t' || *endptr == '\r' || *endptr == '\n') {
    endptr++;
  }
  if (*endptr != '\0') {
    return false;
  }

  if (v < 0 || v > 255) {
    return false;
  }

  *value = (uint8_t)v;
  return true;
}

int main(int argc, char **argv)
{
  // Allocate a buffer using huge page
  // See the handout for details about hugepage management
  void *buf= mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
  
  if (buf == (void*) - 1) {
     perror("mmap() error\n");
     exit(EXIT_FAILURE);
  }
  // The first access to a page triggers overhead associated with
  // page allocation, TLB insertion, etc.
  // Thus, we use a dummy write here to trigger page allocation
  // so later access will not suffer from such overhead.
  //*((char *)buf) = 1; // dummy write to trigger page allocation

  // Warm all addresses used by the channel.
  for (int set_idx = 0; set_idx < NUM_SYMBOL_SETS; set_idx++) {
    for (int way = 0; way < EVICTION_WAYS; way++) {
      *set_addr(buf, set_idx, way) = 1;
    }
  }

  printf("Please type an integer in [0,255] per line (or quit).\n");

  bool sending = true;
  while (sending) {
      char text_buf[128];
      if (fgets(text_buf, sizeof(text_buf), stdin) == NULL) {
        break;
      }

      if (strncmp(text_buf, "quit", 4) == 0 || strncmp(text_buf, "exit", 4) == 0) {
        break;
      }

      uint8_t symbol;
      if (!parse_uint8_line(text_buf, &symbol)) {
        printf("Invalid input. Enter an integer in [0,255].\n");
        continue;
      }

      // Frame each payload with a fixed header symbol so the receiver can
      // distinguish real payloads from noise.
      transmit_symbol(buf, FRAME_HEADER_SET);
      transmit_symbol(buf, symbol);
  }

  printf("Sender finished.\n");
  return 0;
}

