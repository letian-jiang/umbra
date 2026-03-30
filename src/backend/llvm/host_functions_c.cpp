#include "backend/llvm/host_functions_c.h"

#include "codegen/structures/hash_table.h"
#include "codegen/structures/hash_table_layout.h"

using umbra::codegen::HashTable;

// Verify that HashTableFields layout matches the beginning of HashTable.
// HashTable has no virtual functions and no base class, so its first fields
// are laid out in declaration order: slots_, capacity_, numEntries_, entrySize_, slotSize_.
static_assert(sizeof(umbra::HashTableFields) == 24,
              "HashTableFields must be 24 bytes (ptr + 4 x u32)");

// Thin extern "C" wrappers delegating to HashTable member functions.
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)

extern "C" uint8_t* umbra_ht_lookup(void* table, uint64_t hash) {
    return reinterpret_cast<HashTable*>(table)->lookup(hash);
}

extern "C" uint8_t* umbra_ht_insert(void* table, uint64_t hash) {
    return reinterpret_cast<HashTable*>(table)->insert(hash);
}

extern "C" uint8_t* umbra_ht_probe_next(void* table, uint8_t* current_entry, uint64_t hash) {
    return reinterpret_cast<HashTable*>(table)->probeNext(current_entry, hash);
}

extern "C" uint64_t umbra_ht_get_capacity(void* table) {
    return reinterpret_cast<HashTable*>(table)->capacity();
}

extern "C" uint8_t* umbra_ht_get_slot_buffer(void* table) {
    return reinterpret_cast<HashTable*>(table)->slotBuffer();
}

extern "C" uint64_t umbra_ht_get_slot_stride(void* table) {
    return reinterpret_cast<HashTable*>(table)->slotStride();
}

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
