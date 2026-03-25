#pragma once

#include <cstdint>

namespace umbra {

struct RuntimeContext {
    void** columnPtrs{nullptr};
    uint64_t rowCount{0};
    const char* stringBase{nullptr};

    // Hash aggregation (set later by codegen)
    void* hashTable{nullptr};

    // Result output (set later by execution)
    void* resultBuffer{nullptr};
    uint64_t resultRowCount{0};
};

}  // namespace umbra
