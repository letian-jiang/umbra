#pragma once

#include <cstdint>

namespace umbra {

/// POD layout mirroring the first fields of HashTable.
/// Used by the LLVM bitcode inliner (host_inline.cpp) and validated
/// against the real HashTable layout via static_assert in host_functions_c.cpp.
struct HashTableFields {
    uint8_t* slots;
    uint32_t capacity;
    uint32_t numEntries;
    uint32_t entrySize;
    uint32_t slotSize;
};

}  // namespace umbra
