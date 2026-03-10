#include "util.h"
// mman library to be used for hugepage allocations (e.g. mmap or posix_memalign only)
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


#define L2_ASSOCIATIVITY 8
#define L2_SETS 1024 
#define LINE_SIZE 64
#define PAGE_SIZE (1 << 21) // 2 MB
#ifndef SET_STRIDE_BYTES 
#define SET_STRIDE_BYTES (1 << 16) 
#endif

// Calculate Latency Difference through rdtscp
static inline uint64_t rdtscp(void) {
    uint32_t lo, hi;
    asm volatile("rdtscp" : "=a"(lo), "=d"(hi) :: "rcx");
    return ((uint64_t)hi << 32) | lo; // output is the time stamp counter
}

// Linked List so that the CPU will have to follow the chain of pointers, which will cause cache evictions
struct linked_list_node {
    struct linked_list_node *next;
    char padding[LINE_SIZE - sizeof(struct linked_list_node *)];
}

void *work_area;
struct linked_list_node *set_chains[L2_SETS];



int main(int argc, char const *argv[]) {
    int flag = -1;

    // Put your capture-the-flag code here
    
    printf("Flag: %d\n", flag);
    return 0;
}
