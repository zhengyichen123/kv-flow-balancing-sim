#ifndef ASTRA_SIM_COLLECTIVE_TAG_ALLOCATOR_HH
#define ASTRA_SIM_COLLECTIVE_TAG_ALLOCATOR_HH

#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>

namespace AstraSim {

// A TP-only operation must not advance the next EP operation's message tag.
// Disjoint groups with the same dimension mask may reuse tags: matching also
// includes source/destination ranks. Overlapping scopes have distinct residues.
class CollectiveTagAllocator {
 public:
    int next(const std::vector<bool>& dimensions, int rank_dimensions) {
        if (rank_dimensions < 1 || rank_dimensions > 28) {
            throw std::invalid_argument("Unsupported collective topology dimensions");
        }
        const uint64_t radix = uint64_t{1} << rank_dimensions;
        uint64_t mask = 0;
        for (int d = 0; d < rank_dimensions; ++d) {
            if (d < static_cast<int>(dimensions.size()) && dimensions[d]) {
                mask |= uint64_t{1} << d;
            }
        }
        auto& sequence = counters[mask];
        const uint64_t tag = sequence * radix + mask;
        // Sys adds COLLECTIVE=500000000; do not enter its rendezvous range.
        if (tag >= 500000000) {
            throw std::overflow_error("Collective message tag space exhausted");
        }
        ++sequence;
        return static_cast<int>(tag);
    }

 private:
    std::map<uint64_t, uint64_t> counters;
};

}  // namespace AstraSim
#endif
