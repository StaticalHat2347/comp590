#include "utility.h"

#define L1_SIZE 32768
#define L2_SIZE 1048576
#define L3_SIZE 11534336

int main (int ac, char **av) {

    uint64_t dram_latency[SAMPLES] = {0};
    uint64_t l1_latency[SAMPLES] = {0};
    uint64_t l2_latency[SAMPLES] = {0};
    uint64_t l3_latency[SAMPLES] = {0};

    volatile char tmp;

    char *target_buffer = malloc(64);
    assert(target_buffer != NULL);

    char *eviction_buffer = malloc(L3_SIZE);
    assert(eviction_buffer != NULL);

    for (int i = 0; i < SAMPLES; i++) {

        // L1
        tmp = target_buffer[0];
        tmp = target_buffer[0];
        l1_latency[i] = measure_one_block_access_time((uint64_t)target_buffer);

        // L2
        tmp = target_buffer[0];
        for (int j = 0; j < L1_SIZE; j += 64)
            tmp = eviction_buffer[j];
        l2_latency[i] = measure_one_block_access_time((uint64_t)target_buffer);

        // L3
        tmp = target_buffer[0];
        for (int j = 0; j < L2_SIZE; j += 64)
            tmp = eviction_buffer[j];
        l3_latency[i] = measure_one_block_access_time((uint64_t)target_buffer);

        // DRAM
        clflush(target_buffer);
        dram_latency[i] = measure_one_block_access_time((uint64_t)target_buffer);
    }

    print_results_for_python(dram_latency, l1_latency, l2_latency, l3_latency);

    free(target_buffer);
    free(eviction_buffer);

    return 0;
}

