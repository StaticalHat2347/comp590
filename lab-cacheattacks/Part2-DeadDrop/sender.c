#include "util.h"
#include <sys/mman.h>
#include <errno.h>

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

#define BUFF_SIZE (1 << 22)
#define CACHE_LINE_BYTES 64
#define WORKING_SET_LINES (BUFF_SIZE / CACHE_LINE_BYTES)

#define SLOT_NS 180000000ULL
#define PREAMBLE_SLOTS 10
#define GAP_SLOTS 2
#define BIT_REPS 3
#define FRAME_GAP_SLOTS 3

typedef enum {
  TX_MODE_BYTE = 0,
  TX_MODE_SINGLE_BIT = 1,
  TX_MODE_SINGLE_BIT_DIFF = 2
} tx_mode_t;

static inline uint64_t monotonic_ns(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static inline void sleep_ns(uint64_t ns)
{
  struct timespec ts;
  ts.tv_sec = (time_t)(ns / 1000000000ULL);
  ts.tv_nsec = (long)(ns % 1000000000ULL);
  nanosleep(&ts, NULL);
}

static void tx_active_slot(volatile unsigned char *buf)
{
  static volatile unsigned char sink = 0;
  uint64_t end_ns = monotonic_ns() + SLOT_NS;
  unsigned idx = 1;

  while (monotonic_ns() < end_ns) {
    for (int i = 0; i < 32768; i++) {
      idx = (idx * 1103515245u + 12345u) & (WORKING_SET_LINES - 1);
      sink ^= buf[idx * CACHE_LINE_BYTES];
    }
  }
}

static void tx_idle_slot(void)
{
  sleep_ns(SLOT_NS);
}

static void tx_byte(volatile unsigned char *buf, uint8_t value)
{
  for (int i = 0; i < PREAMBLE_SLOTS; i++) {
    tx_active_slot(buf);
  }

  for (int i = 0; i < GAP_SLOTS; i++) {
    tx_idle_slot();
  }

  for (int b = 7; b >= 0; b--) {
    int bit = (value >> b) & 1;
    for (int r = 0; r < BIT_REPS; r++) {
      if (bit) {
        tx_active_slot(buf);
      } else {
        tx_idle_slot();
      }
    }
  }

  for (int i = 0; i < FRAME_GAP_SLOTS; i++) {
    tx_idle_slot();
  }
}

static void tx_bit(volatile unsigned char *buf, int bit)
{
  for (int i = 0; i < PREAMBLE_SLOTS; i++) {
    tx_active_slot(buf);
  }

  for (int i = 0; i < GAP_SLOTS; i++) {
    tx_idle_slot();
  }

  for (int r = 0; r < BIT_REPS; r++) {
    if (bit) {
      tx_active_slot(buf);
    } else {
      tx_idle_slot();
    }
  }

  for (int i = 0; i < FRAME_GAP_SLOTS; i++) {
    tx_idle_slot();
  }
}

static void tx_bit_diff(volatile unsigned char *buf, int bit)
{
  for (int i = 0; i < PREAMBLE_SLOTS; i++) {
    tx_active_slot(buf);
  }

  for (int i = 0; i < GAP_SLOTS; i++) {
    tx_idle_slot();
  }

  for (int r = 0; r < BIT_REPS; r++) {
    if (bit) {
      tx_active_slot(buf);
      tx_idle_slot();
    } else {
      tx_idle_slot();
      tx_active_slot(buf);
    }
  }

  for (int i = 0; i < FRAME_GAP_SLOTS; i++) {
    tx_idle_slot();
  }
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
  if (*endptr != '\0' || v < 0 || v > 255) {
    return false;
  }

  *value = (uint8_t)v;
  return true;
}

static bool parse_bit_line(char *line, int *bit)
{
  uint8_t value;
  if (!parse_uint8_line(line, &value) || value > 1) {
    return false;
  }
  *bit = (int)value;
  return true;
}

static tx_mode_t parse_tx_mode(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--single-bit-diff") == 0) {
      return TX_MODE_SINGLE_BIT_DIFF;
    }
    if (strcmp(argv[i], "--single-bit") == 0) {
      return TX_MODE_SINGLE_BIT;
    }
  }
  return TX_MODE_BYTE;
}

int main(int argc, char **argv)
{
  tx_mode_t mode = parse_tx_mode(argc, argv);
  bool single_bit_mode = (mode != TX_MODE_BYTE);
  bool diff_mode = (mode == TX_MODE_SINGLE_BIT_DIFF);

  int mmap_flags = MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB;
  volatile unsigned char *buf =
      (volatile unsigned char *)mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE, mmap_flags, -1, 0);
  if ((void *)buf == (void *)-1) {
    perror("mmap() error\n");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < BUFF_SIZE; i += CACHE_LINE_BYTES) {
    buf[i] = (unsigned char)(i & 0xFF);
  }

  if (single_bit_mode) {
    if (diff_mode) {
      printf("Single-bit differential mode. Type 0 or 1 per line (or quit).\n");
    } else {
      printf("Single-bit mode. Type 0 or 1 per line (or quit).\n");
    }
  } else {
    printf("Byte mode. Type an integer in [0,255] per line (or quit).\n");
  }

  while (true) {
    char text_buf[128];
    if (fgets(text_buf, sizeof(text_buf), stdin) == NULL) {
      break;
    }
    if (strncmp(text_buf, "quit", 4) == 0 || strncmp(text_buf, "exit", 4) == 0) {
      break;
    }

    if (single_bit_mode) {
      int bit;
      if (!parse_bit_line(text_buf, &bit)) {
        printf("Invalid input. Enter 0 or 1.\n");
        continue;
      }
      if (diff_mode) {
        tx_bit_diff(buf, bit);
      } else {
        tx_bit(buf, bit);
      }
    } else {
      uint8_t value;
      if (!parse_uint8_line(text_buf, &value)) {
        printf("Invalid input. Enter an integer in [0,255].\n");
        continue;
      }
      tx_byte(buf, value);
    }
  }

  printf("Sender finished.\n");
  return 0;
}
