// This file is compiled to LLVM bitcode at system compile time (not as a
// normal C++ source).  The bitcode is embedded in the binary and linked into
// the JIT module at query compile time so LLVM O3 can inline these functions.
//
// Keep this file self-contained — only POD layout headers, no C++ stdlib.

#include <cstdint>
#include <cstring>

// Hash table slot layout constants (must match HashTable::OCCUPIED_OFFSET etc.)
static constexpr uint32_t kOccupiedOffset = 0;
static constexpr uint32_t kHashOffset = 1;
static constexpr uint32_t kPayloadOffset = 9;  // 1 (occupied) + 8 (hash)

// POD mirror of the first fields of HashTable (slots_, capacity_, ... , slotSize_).
// Layout validated by static_assert in host_functions_c.cpp.
struct HTFields {
    uint8_t* slots;
    uint32_t capacity;
    uint32_t numEntries;
    uint32_t entrySize;
    uint32_t slotSize;
};

extern "C" uint8_t* umbra_ht_lookup(void* table_opaque, uint64_t hash) {
    auto* ht = static_cast<HTFields*>(table_opaque);
    uint32_t mask = ht->capacity - 1;
    uint32_t idx = static_cast<uint32_t>(hash) & mask;
    uint32_t stride = ht->slotSize;
    uint8_t* slots = ht->slots;

    for (uint32_t probe = 0; probe < ht->capacity; ++probe) {
        uint32_t slot = (idx + probe) & mask;
        uint8_t* slotPtr = slots + static_cast<uint64_t>(slot) * stride;

        if (slotPtr[kOccupiedOffset] == 0) {
            return nullptr;
        }
        uint64_t storedHash = 0;
        __builtin_memcpy(&storedHash, slotPtr + kHashOffset, 8);
        if (storedHash == hash) {
            return slotPtr + kPayloadOffset;
        }
    }
    return nullptr;
}

extern "C" uint8_t* umbra_ht_probe_next(void* table_opaque, uint8_t* current_entry, uint64_t hash) {
    auto* ht = static_cast<HTFields*>(table_opaque);
    uint32_t mask = ht->capacity - 1;
    uint32_t stride = ht->slotSize;
    uint8_t* slots = ht->slots;

    auto* currentSlotPtr = current_entry - kPayloadOffset;
    auto byteOffset = static_cast<uint64_t>(currentSlotPtr - slots);
    auto currentSlot = static_cast<uint32_t>(byteOffset / stride);

    for (uint32_t probe = 1; probe < ht->capacity; ++probe) {
        uint32_t slot = (currentSlot + probe) & mask;
        uint8_t* slotPtr = slots + static_cast<uint64_t>(slot) * stride;

        if (slotPtr[kOccupiedOffset] == 0) {
            return nullptr;
        }
        uint64_t storedHash = 0;
        __builtin_memcpy(&storedHash, slotPtr + kHashOffset, 8);
        if (storedHash == hash) {
            return slotPtr + kPayloadOffset;
        }
    }
    return nullptr;
}
