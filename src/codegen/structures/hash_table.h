#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/tuples/tuple_layout.h"

namespace umbra::codegen {

/// HashTable — Layer 2 runtime data structure for hash aggregation.
///
/// Open addressing with linear probing. Each slot has the layout:
///   [occupied(1B) | hash(8B) | payload(variable)]
/// where payload = group-by keys + aggregate states, packed via TupleLayout.
///
/// This is a RUNTIME C++ class. The codegen interface (HashTableCodegen)
/// emits IR that calls into host functions on this class.
class HashTable {
   public:
    /// Construct a hash table with the given payload (entry) size.
    /// @param entrySize Byte size of the payload portion (keys + agg states).
    explicit HashTable(uint32_t entrySize);

    // Move-only.
    HashTable(const HashTable&) = delete;
    HashTable& operator=(const HashTable&) = delete;
    HashTable(HashTable&&) noexcept;
    HashTable& operator=(HashTable&&) noexcept;

    ~HashTable();

    /// Find a slot matching the given hash, or return nullptr if not found.
    /// Performs linear probing from the initial position.
    /// @return Pointer to the payload of the matching slot, or nullptr.
    [[nodiscard]] uint8_t* lookup(uint64_t hashVal) const;

    /// Insert a new entry with the given hash. The slot is marked occupied.
    /// @return Pointer to the payload of the newly inserted slot.
    uint8_t* insert(uint64_t hashVal);

    /// Advance to the next slot during linear probing.
    /// @param currentEntry Pointer to the current payload.
    /// @param hashVal The hash being searched for.
    /// @return Pointer to the next slot's payload matching hash, or nullptr.
    [[nodiscard]] uint8_t* probeNext(uint8_t* currentEntry, uint64_t hashVal) const;

    /// Get the number of occupied entries.
    [[nodiscard]] uint32_t size() const { return numEntries_; }

    /// Get the capacity (total number of slots).
    [[nodiscard]] uint64_t capacity() const { return capacity_; }

    /// Get the entry size (payload size per slot).
    [[nodiscard]] uint32_t entrySize() const { return entrySize_; }

    /// Get a pointer to the raw slot buffer (for codegen iteration).
    [[nodiscard]] uint8_t* slotBuffer() const { return slots_; }

    /// Get the byte stride per slot.
    [[nodiscard]] uint32_t slotStride() const { return slotSize_; }

    /// Iterate over all occupied entries, calling the callback with each payload pointer.
    void iterate(void (*callback)(uint8_t* entry, void* userData), void* userData) const;

    // ── Static host function wrappers (called from generated IR) ─────────

    /// Host function: lookup(table_ptr, hash) -> entry_ptr or 0.
    static uint64_t hostLookup(const uint64_t* args, uint8_t count);

    /// Host function: insert(table_ptr, hash) -> entry_ptr.
    static uint64_t hostInsert(const uint64_t* args, uint8_t count);

    /// Host function: probeNext(table_ptr, entry_ptr, hash) -> entry_ptr or 0.
    static uint64_t hostProbeNext(const uint64_t* args, uint8_t count);

    /// Host function: getCapacity(table_ptr) -> capacity.
    static uint64_t hostGetCapacity(const uint64_t* args, uint8_t count);

    /// Host function: getSlotBuffer(table_ptr) -> slot buffer ptr.
    static uint64_t hostGetSlotBuffer(const uint64_t* args, uint8_t count);

    /// Host function: getSlotStride(table_ptr) -> stride.
    static uint64_t hostGetSlotStride(const uint64_t* args, uint8_t count);

    /// Slot layout constants — public for codegen access.
    static constexpr uint32_t OCCUPIED_OFFSET = 0;
    static constexpr uint32_t HASH_OFFSET = 1;
    static constexpr uint32_t PAYLOAD_OFFSET = 9;  // 1 + 8

   private:
    /// Minimum initial capacity (must be power of 2).
    static constexpr uint32_t INITIAL_CAPACITY = 16;

    /// Load factor threshold for resize (percentage, e.g. 70 = 70%).
    static constexpr uint32_t LOAD_FACTOR_PERCENT = 70;

    uint8_t* slots_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t numEntries_ = 0;
    uint32_t entrySize_ = 0;
    uint32_t slotSize_ = 0;

    /// Allocate the slot buffer.
    void allocate(uint32_t newCapacity);

    /// Resize and rehash when load factor exceeded.
    void resize();

    /// Get a pointer to the start of slot i.
    [[nodiscard]] uint8_t* slotAt(uint32_t index) const;

    /// Check if a slot is occupied.
    [[nodiscard]] bool isOccupied(uint32_t index) const;

    /// Get the stored hash at a slot.
    [[nodiscard]] uint64_t storedHash(uint32_t index) const;

    /// Get the payload pointer for a slot.
    [[nodiscard]] uint8_t* payloadAt(uint32_t index) const;

    /// Compute the slot index from a hash value.
    [[nodiscard]] uint32_t slotIndex(uint64_t hashVal) const;
};

/// HashTableCodegen — codegen interface for HashTable (emits IR).
///
/// Used by the HashAggregateOperator to emit IR code that interacts
/// with a runtime HashTable instance via host function calls.
class HashTableCodegen {
   public:
    /// Construct a codegen interface for a hash table.
    /// @param keyTypes SQL types of the group-by keys.
    /// @param aggTypes SQL types of the aggregate states.
    HashTableCodegen(const std::vector<SQLType>& keyTypes, const std::vector<SQLType>& aggTypes);

    /// Get the key layout.
    [[nodiscard]] const TupleLayout& keyLayout() const { return keyLayout_; }

    /// Get the aggregate state layout.
    [[nodiscard]] const TupleLayout& aggLayout() const { return aggLayout_; }

    /// Get the total entry size (keys + agg states).
    [[nodiscard]] uint32_t entrySize() const {
        return keyLayout_.totalSize() + aggLayout_.totalSize();
    }

    /// Get the aggregate state offset within an entry.
    [[nodiscard]] uint32_t aggOffset() const { return keyLayout_.totalSize(); }

    /// Register host functions on the codegen context.
    /// Must be called before emitting findOrInsert/scan.
    void registerHostFunctions(CodegenContext& ctx);

    /// Emit IR for find-or-insert pattern.
    /// Uses a loop: lookup -> compare keys -> probeNext or insert.
    /// @return Pointer to the entry payload (Ptr<Int8>).
    Ptr<Int8> findOrInsert(CodegenContext& ctx, Ptr<Int8> tablePtr, UInt64 hashVal,
                           const std::vector<SQLValue>& keys);

    /// Emit IR to iterate over entries in the hash table.
    /// Generates a loop over slots [startSlot, endSlot), checking the occupied flag,
    /// and calling the callback for each occupied entry.
    void scan(CodegenContext& ctx, Ptr<Int8> tablePtr, Int64 startSlot, Int64 endSlot,
              const std::function<void(Ptr<Int8>)>& callback);

   private:
    TupleLayout keyLayout_;
    TupleLayout aggLayout_;

    ir::FunctionId lookupFn_ = UINT32_MAX;
    ir::FunctionId insertFn_ = UINT32_MAX;
    ir::FunctionId probeNextFn_ = UINT32_MAX;
    ir::FunctionId getCapacityFn_ = UINT32_MAX;
    ir::FunctionId getSlotBufferFn_ = UINT32_MAX;
    ir::FunctionId getSlotStrideFn_ = UINT32_MAX;

    /// Emit key comparison IR: compare unpacked keys with input keys.
    /// Returns a Bool that is true if all keys match.
    static Bool emitKeyComparison(CodegenContext& ctx, const std::vector<SQLValue>& stored,
                                  const std::vector<SQLValue>& input);
};

}  // namespace umbra::codegen
