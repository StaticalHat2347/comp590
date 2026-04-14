#include <algorithm>
#include <array>
#include <tuple>
#include <ranges>
#include <numeric>

#include "../shared.hh"
#include "../params.hh"
#include "../util.hh"

#define BANKS 16
#define CONSISTENCY_RATE 0.90
// TODO: Threshold derived in part2
#define THRESHOLD 370 
#define POOL_SIZE 1000
#define ROUNDS  100





void print_bins(const std::array<std::vector<uint64_t>, BANKS>& bins) {
    for (size_t i = 0; i < BANKS; i++) {
        const auto& bin = bins[i];
        printf("Bin[%zu] size = %zu\n", i, bin.size());
        for (size_t j = 0; j < bin.size(); j++) {
            printf("  [%3zu] phys = 0x%lx\n", j, bin[j]);
        }
        printf("\n");
    }
}

int gt(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

uint64_t median(uint64_t* vals, size_t size) {
    qsort(vals, size, sizeof(uint64_t), gt);
    if (size % 2 == 1) {
        return vals[size / 2];
    } else {
        return (vals[size / 2 - 1] + vals[size / 2]) / 2;
    }
}
/*
 * bin_rows
 *
 * Bins a selection of addresses in the range of [starting_addr, final_addr)
 * based on measure_bank_latency.
 *
 * Input: starting and ending addresses
 * Output: An array of 16 vectors, each of which holds addresses that share the same bank
 *
 * HINT: You can refer the way in part2.cc: run measure_bank_latency for multiple times and then get the median number to avoid noise
 *
 */

std::array<std::vector<uint64_t>, BANKS> bin_rows(uint64_t starting_addr, uint64_t final_addr) {
    // TODO - Exercise 3-1
    std::array<std::vector<uint64_t>, BANKS> bins;

    
    size_t next_empty_bin = 0;

    for (uint64_t virt = starting_addr; virt < final_addr; virt += ROW_SIZE) {
        uint64_t phys = virt_to_phys(virt);
        if (phys == 0) continue;

        bool placed = false;

        for (size_t i = 0; i < next_empty_bin; i++) {
            if (bins[i].empty()) continue;

            uint64_t rep_phys = bins[i][0];
            uint64_t rep_virt = phys_to_virt(rep_phys);
            if (rep_virt == 0) continue;

            uint64_t vals[ROUNDS];
            for (size_t r = 0; r < ROUNDS; r++) {
                vals[r] = measure_bank_latency(
                    reinterpret_cast<volatile char*>(virt),
                    reinterpret_cast<volatile char*>(rep_virt)
                );
            }

            uint64_t med = median(vals, ROUNDS);

            if (med > THRESHOLD) {
                bins[i].push_back(phys);
                placed = true;
                break;
            }
        }

        if (!placed && next_empty_bin < BANKS) {
            bins[next_empty_bin].push_back(phys);
            next_empty_bin++;
        }
    }

    return bins;
}

/*
 *
 * DO NOT MODIFY BELOW ME
 *
 */

std::tuple<uint64_t, double> get_most_frequent(const std::vector<uint64_t>& data) {
    std::map<uint64_t, uint64_t> freq_map;

    for (const auto& item : data) {
        freq_map[item]++;
    }

    auto [most_freq, max_count] = std::accumulate(
        freq_map.begin(),
        freq_map.end(),
        std::pair<uint64_t, uint64_t>{0, 0},
        [](const auto& best, const auto& current) {
            return current.second > best.second ? 
                std::pair{current.first, current.second} : best;
        }
    );

    return {most_freq, static_cast<double>(max_count) / data.size()};
}



template <size_t LEN>
std::optional<uint64_t> find_candidate_function(const std::array<std::vector<uint64_t>, LEN>& bins) {
    std::optional<uint64_t> result = std::nullopt;

    std::array<std::function<uint64_t(uint64_t)>, 3> functions = {
        [](uint64_t x) {
            return ((get_bit(x, 14) ^ get_bit(x, 17)) << 3) | 
                   ((get_bit(x, 15) ^ get_bit(x, 18)) << 2) | 
                   ((get_bit(x, 16) ^ get_bit(x, 19)) << 1) |
                   ((get_bit(x, 7) ^ get_bit(x, 8) ^ get_bit(x, 9) ^ get_bit(x, 12) ^ get_bit(x, 13) ^ get_bit(x, 15) ^ get_bit(x, 16)));
        },
        [](uint64_t x) {
            return ((get_bit(x, 15) ^ get_bit(x, 18)) << 3) | 
                   ((get_bit(x, 16) ^ get_bit(x, 19)) << 2) | 
                   ((get_bit(x, 17) ^ get_bit(x, 20)) << 1) |
                   ((get_bit(x, 7) ^ get_bit(x, 8) ^ get_bit(x, 9) ^ get_bit(x, 12) ^ get_bit(x, 13) ^ get_bit(x, 18) ^ get_bit(x, 19)));
        },

        [](uint64_t x) {
            return ((get_bit(x, 13) ^ get_bit(x, 17)) << 3) | 
                   ((get_bit(x, 14) ^ get_bit(x, 18)) << 2) | 
                   ((get_bit(x, 15) ^ get_bit(x, 19)) << 1) |
                   ((get_bit(x, 7) ^ get_bit(x, 8) ^ get_bit(x, 9) ^ get_bit(x, 12) ^ get_bit(x, 13) ^ get_bit(x, 20) ^ get_bit(x, 21)));
        },
                
    };

    for (size_t i = 0; i < functions.size(); i++) {
        auto &f = functions[i];
        bool good = true;
        for (auto &bin: bins) {
            uint64_t match_count = 0;
            std::vector<uint64_t> bank_comp(bin.size());
            std::transform(bin.begin(), bin.end(), bank_comp.begin(), f);
            auto [id, freq] = get_most_frequent(bank_comp);
            if (freq < CONSISTENCY_RATE) {
                good = false;
                break;
            }
        }
        if (good) {
            // conflicting results
            if (result.has_value())
                return std::nullopt;
            result = i;
        }
    }

    return result;
}

int main (int ac, char **av) {
    
    setvbuf(stdout, NULL, _IONBF, 0);

    // Allocate a large pool of memory (of size BUFFER_SIZE_MB) pointed to
    // by allocated_mem
    allocated_mem = allocate_pages(BUFFER_SIZE_MB * 1024UL * 1024UL);
 
    // Setup PPN_VPN_map
    setup_PPN_VPN_map(allocated_mem, PPN_VPN_map);

    auto result = find_candidate_function(
        bin_rows((uint64_t)allocated_mem, (uint64_t)allocated_mem + ROW_SIZE * 4096));

    if (result.has_value()) {
        printf("Identified function %lu as correct\n", result.value());
    } else {
        puts("Did not identify a correct function :(");
    }
}