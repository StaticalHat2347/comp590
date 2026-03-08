#include "util.h"
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#define BUFF_SIZE (1 << 21)
#define SET_ASSOCIATIVITY 16
#ifndef SET_STRIDE_BYTES
#define SET_STRIDE_BYTES (1 << 16)
#endif

#define SET_SPACING 32
#define BASE_SET 64
#define VALID_SET_ID 8
#define DATA_BITS 8
#define SAMPLE_WINDOW 100
#define VALID_MIN_COUNT (SAMPLE_WINDOW / 2)
#define DROP_STREAK_TARGET 10
#define DROP_WAIT_CAP 2000000

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

static inline uint64_t rdtscp(void) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
#endif
}

struct node {
    struct node *next;
    char pad[64 - sizeof(struct node *)];
};

static void *work_area;
static struct node *set_chains[256];
static uint64_t set_thresholds[256];

enum rx_phase {
    RX_WAIT_VALID = 0,
    RX_SAMPLE,
    RX_DECODE,
    RX_WAIT_DROP
};

static void tiny_pause(int count) {
    for (volatile int k = 0; k < count; k++) {
    }
}

static void shuffle_nodes(struct node **nodes, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct node *tmp = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = tmp;
    }
}

static void create_set_chain(int logical_set_id) {
    char *base = (char *)work_area;
    struct node *ways[SET_ASSOCIATIVITY];
    int physical_set_id = BASE_SET + logical_set_id * SET_SPACING;

    for (int way = 0; way < SET_ASSOCIATIVITY; way++) {
        ways[way] = (struct node *)(base + physical_set_id * 64 + way * SET_STRIDE_BYTES);
    }

    shuffle_nodes(ways, SET_ASSOCIATIVITY);

    for (int way = 0; way < SET_ASSOCIATIVITY - 1; way++) {
        ways[way]->next = ways[way + 1];
    }
    ways[SET_ASSOCIATIVITY - 1]->next = NULL;

    set_chains[logical_set_id] = ways[0];
}

static void prime_set(int set_id) {
    struct node *cursor = set_chains[set_id];
    while (cursor) {
        cursor = cursor->next;
    }
}

static uint64_t probe_set(int set_id) {
    uint64_t t0 = rdtscp();
    struct node *cursor = set_chains[set_id];
    while (cursor) {
        cursor = cursor->next;
    }
    return rdtscp() - t0;
}

static int is_active_set(int set_id) {
    return probe_set(set_id) > set_thresholds[set_id];
}

static void initialize_memory_and_sets(void) {
    work_area = mmap(NULL,
                     BUFF_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB,
                     -1,
                     0);

    if (work_area == (void *)-1) {
        perror("mmap() error\n");
        exit(EXIT_FAILURE);
    }

    *((char *)work_area) = 1;

    for (int set_id = 0; set_id <= VALID_SET_ID; set_id++) {
        create_set_chain(set_id);
    }
}

static void calibrate_thresholds(uint64_t manual_limit) {
    for (int set_id = 0; set_id <= VALID_SET_ID; set_id++) {
        if (manual_limit > 0) {
            set_thresholds[set_id] = manual_limit;
            continue;
        }

        uint64_t sum = 0;
        const int reps = 1000;
        for (int i = 0; i < reps; i++) {
            prime_set(set_id);
            sum += probe_set(set_id);
        }
        set_thresholds[set_id] = (sum / reps) * 2;
    }
}

static int detect_valid_start(void) {
    prime_set(VALID_SET_ID);
    tiny_pause(2000);
    return is_active_set(VALID_SET_ID);
}

static int collect_votes(int votes[DATA_BITS]) {
    int valid_hits = 0;

    for (int b = 0; b < DATA_BITS; b++) {
        votes[b] = 0;
    }

    for (int sample = 0; sample < SAMPLE_WINDOW; sample++) {
        for (int set_id = 0; set_id <= VALID_SET_ID; set_id++) {
            prime_set(set_id);
        }

        tiny_pause(5000);

        if (!is_active_set(VALID_SET_ID)) {
            continue;
        }

        valid_hits++;
        for (int bit = 0; bit < DATA_BITS; bit++) {
            if (is_active_set(bit)) {
                votes[bit]++;
            }
        }
    }

    return valid_hits;
}

static int decode_byte_from_votes(const int votes[DATA_BITS], int valid_hits) {
    int decoded = 0;

    for (int bit = 0; bit < DATA_BITS; bit++) {
        if (votes[bit] * 4 > valid_hits * 3) {
            decoded |= (1 << bit);
        }
    }

    return decoded;
}

static void wait_for_drop(void) {
    int low_streak = 0;
    int attempts = 0;

    while (attempts < DROP_WAIT_CAP) {
        prime_set(VALID_SET_ID);
        tiny_pause(5000);

        if (!is_active_set(VALID_SET_ID)) {
            low_streak++;
            if (low_streak >= DROP_STREAK_TARGET) {
                return;
            }
        } else {
            low_streak = 0;
        }

        attempts++;
    }
}

int main(int argc, char **argv) {
    srand(time(NULL));

    uint64_t manual_limit = 0;
    if (argc > 1) {
        manual_limit = strtoull(argv[1], NULL, 10);
    }

    initialize_memory_and_sets();
    calibrate_thresholds(manual_limit);

    printf("Please press enter.\n");
    char enter_buf[2];
    fgets(enter_buf, sizeof(enter_buf), stdin);
    printf("Receiver now listening.\n");

    enum rx_phase phase = RX_WAIT_VALID;
    int votes[DATA_BITS] = {0};
    int valid_hits = 0;
    int decoded = 0;

    while (true) {
        switch (phase) {
            case RX_WAIT_VALID:
                phase = detect_valid_start() ? RX_SAMPLE : RX_WAIT_VALID;
                break;

            case RX_SAMPLE:
                valid_hits = collect_votes(votes);
                phase = RX_DECODE;
                break;

            case RX_DECODE:
                if (valid_hits > VALID_MIN_COUNT) {
                    decoded = decode_byte_from_votes(votes, valid_hits);
                    printf("Received: %d\n", decoded);
                    phase = RX_WAIT_DROP;
                } else {
                    phase = RX_WAIT_VALID;
                }
                break;

            case RX_WAIT_DROP:
                wait_for_drop();
                phase = RX_WAIT_VALID;
                break;
        }
    }

    return 0;
}
