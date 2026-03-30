#pragma once

#include <cstdint>

// extern "C" wrappers for host functions called from LLVM-JIT'd code.
// These provide stable C ABI signatures so the LLVM backend can call them
// directly (no trampoline, no arg-array marshaling).

extern "C" {

/// Hash table lookup: returns payload pointer or nullptr.
uint8_t* umbra_ht_lookup(void* table, uint64_t hash);

/// Hash table insert: returns payload pointer of new entry.
uint8_t* umbra_ht_insert(void* table, uint64_t hash);

/// Hash table probe next: linear probe to next matching slot.
uint8_t* umbra_ht_probe_next(void* table, uint8_t* current_entry, uint64_t hash);

/// Hash table get capacity.
uint64_t umbra_ht_get_capacity(void* table);

/// Hash table get slot buffer pointer.
uint8_t* umbra_ht_get_slot_buffer(void* table);

/// Hash table get slot stride (bytes per slot).
uint64_t umbra_ht_get_slot_stride(void* table);

}  // extern "C"
