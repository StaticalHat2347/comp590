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
#define SEND_HOLD_ITERS 2000000L

#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0
#endif

struct node {
    struct node *next;
    char pad[64 - sizeof(struct node *)];
};

static void *work_area;
static struct node *set_chains[256];

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

static void touch_chain_once(int set_id) {
    struct node *cursor = set_chains[set_id];
    while (cursor) {
        cursor = cursor->next;
    }
}

static void evict_set(int set_id) {
    touch_chain_once(set_id);
    touch_chain_once(set_id);
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

static int parse_byte_input(const char *line, int *value_out) {
    char *end = NULL;
    long value = strtol(line, &end, 10);

    if (end == line) {
        return 0;
    }
    while (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r') {
        end++;
    }
    if (*end != '\0' || value < 0 || value > 255) {
        return 0;
    }

    *value_out = (int)value;
    return 1;
}

static int build_active_set_list(int payload, int active_sets[DATA_BITS]) {
    int count = 0;
    uint8_t mask = (uint8_t)payload;

    for (int set_id = 0; set_id < DATA_BITS; set_id++) {
        if (mask & (uint8_t)(1u << set_id)) {
            active_sets[count++] = set_id;
        }
    }
    return count;
}

static void transmit_payload(int payload) {
    int active_sets[DATA_BITS];
    int active_count = build_active_set_list(payload, active_sets);

    for (long iter = 0; iter < SEND_HOLD_ITERS; iter++) {
        evict_set(VALID_SET_ID);
        for (int idx = 0; idx < active_count; idx++) {
            evict_set(active_sets[idx]);
        }
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    srand(time(NULL));
    initialize_memory_and_sets();

    printf("Please type a message.\n");

    for (;;) {
        char input_buf[128];
        int payload = 0;

        if (fgets(input_buf, sizeof(input_buf), stdin) == NULL) {
            break;
        }

        if (!parse_byte_input(input_buf, &payload)) {
            printf("Please enter a value between 0 and 255.\n");
            continue;
        }

        transmit_payload(payload);
        printf("Sent. Please type a message.\n");
    }

    return 0;
}
