#include "codegen/structures/hash_table.h"

#include <cassert>
#include <cstring>

#ifdef UMBRA_LLVM_BACKEND
#include "backend/llvm/host_functions_c.h"
#endif

namespace umbra::codegen {

// ── HashTable (runtime) ─────────────────────────────────────────────────────

HashTable::HashTable(uint32_t entrySize)
    : entrySize_(entrySize), slotSize_(PAYLOAD_OFFSET + entrySize) {
    allocate(INITIAL_CAPACITY);
}

HashTable::HashTable(HashTable&& other) noexcept
    : slots_(other.slots_),
      capacity_(other.capacity_),
      numEntries_(other.numEntries_),
      entrySize_(other.entrySize_),
      slotSize_(other.slotSize_) {
    other.slots_ = nullptr;
    other.capacity_ = 0;
    other.numEntries_ = 0;
}

HashTable& HashTable::operator=(HashTable&& other) noexcept {
    if (this != &other) {
        delete[] slots_;
        slots_ = other.slots_;
        capacity_ = other.capacity_;
        numEntries_ = other.numEntries_;
        entrySize_ = other.entrySize_;
        slotSize_ = other.slotSize_;
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.numEntries_ = 0;
    }
    return *this;
}

HashTable::~HashTable() {
    delete[] slots_;
}

void HashTable::allocate(uint32_t newCapacity) {
    auto bufSize = static_cast<size_t>(newCapacity) * slotSize_;
    slots_ = new uint8_t[bufSize];
    std::memset(slots_, 0, bufSize);
    capacity_ = newCapacity;
}

uint8_t* HashTable::slotAt(uint32_t index) const {
    return slots_ + static_cast<size_t>(index) * slotSize_;
}

bool HashTable::isOccupied(uint32_t index) const {
    return slotAt(index)[OCCUPIED_OFFSET] != 0;
}

uint64_t HashTable::storedHash(uint32_t index) const {
    uint64_t h = 0;
    std::memcpy(&h, slotAt(index) + HASH_OFFSET, sizeof(uint64_t));
    return h;
}

uint8_t* HashTable::payloadAt(uint32_t index) const {
    return slotAt(index) + PAYLOAD_OFFSET;
}

uint32_t HashTable::slotIndex(uint64_t hashVal) const {
    return static_cast<uint32_t>(hashVal & (capacity_ - 1));
}

uint8_t* HashTable::lookup(uint64_t hashVal) const {
    auto idx = slotIndex(hashVal);
    for (uint32_t probe = 0; probe < capacity_; ++probe) {
        auto slot = (idx + probe) & (capacity_ - 1);
        if (!isOccupied(slot)) {
            return nullptr;
        }
        if (storedHash(slot) == hashVal) {
            return payloadAt(slot);
        }
    }
    return nullptr;
}

uint8_t* HashTable::insert(uint64_t hashVal) {
    // Check if we need to resize.
    if ((numEntries_ + 1) * 100 > capacity_ * LOAD_FACTOR_PERCENT) {
        resize();
    }

    auto idx = slotIndex(hashVal);
    for (uint32_t probe = 0; probe < capacity_; ++probe) {
        auto slot = (idx + probe) & (capacity_ - 1);
        if (!isOccupied(slot)) {
            auto* slotPtr = slotAt(slot);
            slotPtr[OCCUPIED_OFFSET] = 1;
            std::memcpy(slotPtr + HASH_OFFSET, &hashVal, sizeof(uint64_t));
            // Zero-initialize the payload.
            std::memset(slotPtr + PAYLOAD_OFFSET, 0, entrySize_);
            ++numEntries_;
            return payloadAt(slot);
        }
    }
    // Should never happen if load factor is maintained.
    assert(false && "Hash table full");
    return nullptr;
}

uint8_t* HashTable::probeNext(uint8_t* currentEntry, uint64_t hashVal) const {
    auto* currentSlotPtr = currentEntry - PAYLOAD_OFFSET;
    auto byteOffset = static_cast<size_t>(currentSlotPtr - slots_);
    auto currentSlot = static_cast<uint32_t>(byteOffset / slotSize_);

    for (uint32_t probe = 1; probe < capacity_; ++probe) {
        auto slot = (currentSlot + probe) & (capacity_ - 1);
        if (!isOccupied(slot)) {
            return nullptr;
        }
        if (storedHash(slot) == hashVal) {
            return payloadAt(slot);
        }
    }
    return nullptr;
}

void HashTable::resize() {
    auto oldCapacity = capacity_;
    auto* oldSlots = slots_;
    auto oldSlotSize = slotSize_;
    // Reset numEntries_ since insert() will increment it for each re-inserted entry.
    numEntries_ = 0;
    allocate(oldCapacity * 2);

    for (uint32_t i = 0; i < oldCapacity; ++i) {
        auto* oldSlot = oldSlots + static_cast<size_t>(i) * oldSlotSize;
        if (oldSlot[OCCUPIED_OFFSET] != 0) {
            uint64_t h = 0;
            std::memcpy(&h, oldSlot + HASH_OFFSET, sizeof(uint64_t));
            auto* newEntry = insert(h);
            std::memcpy(newEntry, oldSlot + PAYLOAD_OFFSET, entrySize_);
        }
    }

    delete[] oldSlots;
}

void HashTable::iterate(void (*callback)(uint8_t* entry, void* userData), void* userData) const {
    for (uint32_t i = 0; i < capacity_; ++i) {
        if (isOccupied(i)) {
            callback(payloadAt(i), userData);
        }
    }
}

// ── Host function wrappers ──────────────────────────────────────────────────

uint64_t HashTable::hostLookup(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    auto* result = table->lookup(args[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uint64_t>(result);
}

uint64_t HashTable::hostInsert(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    auto* result = table->insert(args[1]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uint64_t>(result);
}

uint64_t HashTable::hostProbeNext(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* currentEntry = reinterpret_cast<uint8_t*>(args[1]);
    auto* result = table->probeNext(currentEntry, args[2]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uint64_t>(result);
}

uint64_t HashTable::hostGetCapacity(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    return table->capacity();
}

uint64_t HashTable::hostGetSlotBuffer(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uint64_t>(table->slotBuffer());
}

uint64_t HashTable::hostGetSlotStride(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* table = reinterpret_cast<HashTable*>(args[0]);
    return table->slotStride();
}

// ── HashTableCodegen ────────────────────────────────────────────────────────

HashTableCodegen::HashTableCodegen(const std::vector<SQLType>& keyTypes,
                                   const std::vector<SQLType>& aggTypes)
    : keyLayout_(keyTypes), aggLayout_(aggTypes) {}

void HashTableCodegen::registerHostFunctions(CodegenContext& ctx) {
#ifdef UMBRA_LLVM_BACKEND
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    lookupFn_ = ctx.registerHostFunction("ht_lookup", HashTable::hostLookup, ir::TypeId::PTR,
                                         {ir::TypeId::PTR, ir::TypeId::UINT64},
                                         reinterpret_cast<void*>(&umbra_ht_lookup), true);
    insertFn_ = ctx.registerHostFunction("ht_insert", HashTable::hostInsert, ir::TypeId::PTR,
                                         {ir::TypeId::PTR, ir::TypeId::UINT64},
                                         reinterpret_cast<void*>(&umbra_ht_insert), false);
    probeNextFn_ =
        ctx.registerHostFunction("ht_probe_next", HashTable::hostProbeNext, ir::TypeId::PTR,
                                 {ir::TypeId::PTR, ir::TypeId::PTR, ir::TypeId::UINT64},
                                 reinterpret_cast<void*>(&umbra_ht_probe_next), true);
    getCapacityFn_ = ctx.registerHostFunction(
        "ht_capacity", HashTable::hostGetCapacity, ir::TypeId::UINT64, {ir::TypeId::PTR},
        reinterpret_cast<void*>(&umbra_ht_get_capacity), false);
    getSlotBufferFn_ = ctx.registerHostFunction(
        "ht_slot_buffer", HashTable::hostGetSlotBuffer, ir::TypeId::PTR, {ir::TypeId::PTR},
        reinterpret_cast<void*>(&umbra_ht_get_slot_buffer), false);
    getSlotStrideFn_ = ctx.registerHostFunction(
        "ht_slot_stride", HashTable::hostGetSlotStride, ir::TypeId::UINT64, {ir::TypeId::PTR},
        reinterpret_cast<void*>(&umbra_ht_get_slot_stride), false);
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
#else
    lookupFn_ = ctx.registerHostFunction("ht_lookup", HashTable::hostLookup, ir::TypeId::PTR,
                                         {ir::TypeId::PTR, ir::TypeId::UINT64});
    insertFn_ = ctx.registerHostFunction("ht_insert", HashTable::hostInsert, ir::TypeId::PTR,
                                         {ir::TypeId::PTR, ir::TypeId::UINT64});
    probeNextFn_ =
        ctx.registerHostFunction("ht_probe_next", HashTable::hostProbeNext, ir::TypeId::PTR,
                                 {ir::TypeId::PTR, ir::TypeId::PTR, ir::TypeId::UINT64});
    getCapacityFn_ = ctx.registerHostFunction("ht_capacity", HashTable::hostGetCapacity,
                                              ir::TypeId::UINT64, {ir::TypeId::PTR});
    getSlotBufferFn_ = ctx.registerHostFunction("ht_slot_buffer", HashTable::hostGetSlotBuffer,
                                                ir::TypeId::PTR, {ir::TypeId::PTR});
    getSlotStrideFn_ = ctx.registerHostFunction("ht_slot_stride", HashTable::hostGetSlotStride,
                                                ir::TypeId::UINT64, {ir::TypeId::PTR});
#endif
}

Bool HashTableCodegen::emitKeyComparison(CodegenContext& ctx, const std::vector<SQLValue>& stored,
                                         const std::vector<SQLValue>& input) {
    assert(stored.size() == input.size());

    auto allMatch = ctx.constBool(true);
    for (size_t i = 0; i < stored.size(); ++i) {
        auto cmp = SQLValue::evaluateCompare(ctx, CmpOp::EQ, stored[i], input[i]);
        allMatch = ctx.land(allMatch, Bool(cmp.value()));
    }
    return allMatch;
}

Ptr<Int8> HashTableCodegen::findOrInsert(CodegenContext& ctx, Ptr<Int8> tablePtr, UInt64 hashVal,
                                         const std::vector<SQLValue>& keys) {
    // Flat strategy using select instructions instead of nested if/else.
    // This avoids SSA dominance issues with deeply nested blocks.
    //
    // Loop vars: [entry_ptr (PTR)]
    // Each iteration:
    //   1. If entry is null: insert new, pack keys, return (exit loop).
    //   2. If entry is not null and keys match: return (exit loop).
    //   3. Otherwise: probeNext, continue loop.

    auto& bld = ctx.builder();
    auto nullConst = ctx.constNull<Int8>();
    auto initEntry = ctx.call<Ptr<Int8>>(lookupFn_, tablePtr, hashVal);

    // Check if initial lookup found nothing — fast path for new groups.
    auto initIsNull = Bool(bld.emitCmpEq(ir::TypeId::PTR, initEntry.op(), nullConst.op()));

    IfBuilder ifInitNull(ctx, initIsNull);
    // Fast path: no entry with this hash. Insert directly.
    auto fastEntry = ctx.call<Ptr<Int8>>(insertFn_, tablePtr, hashVal);
    keyLayout_.pack(ctx, fastEntry, keys);

    ifInitNull.elseBlock();
    // Slow path: entry exists. Need to check keys, possibly probe.
    // First check: do the keys match on this first candidate?
    auto firstStoredKeys = keyLayout_.unpack(ctx, initEntry);
    auto firstMatch = emitKeyComparison(ctx, firstStoredKeys, keys);

    IfBuilder ifFirstMatch(ctx, firstMatch);
    // Keys match on first try. Return initEntry.
    auto firstMatchEntry = initEntry;

    ifFirstMatch.elseBlock();
    // Keys don't match. Enter probe loop.
    // Loop: probe next slots until we find a match or an empty slot.
    auto initProbe = ctx.call<Ptr<Int8>>(probeNextFn_, tablePtr, initEntry, hashVal);

    LoopBuilder loop(ctx, {initProbe.op()}, {ir::TypeId::PTR});
    auto probeEntry = loop.getLoopVar<Ptr<Int8>>(0);

    // Is probe entry null?
    auto probeIsNull = Bool(bld.emitCmpEq(ir::TypeId::PTR, probeEntry.op(), nullConst.op()));

    IfBuilder ifProbeNull(ctx, probeIsNull);
    // Null: insert new entry.
    auto insertedEntry = ctx.call<Ptr<Int8>>(insertFn_, tablePtr, hashVal);
    keyLayout_.pack(ctx, insertedEntry, keys);
    auto insertDone = ctx.constBool(true);

    ifProbeNull.elseBlock();
    // Not null: check keys.
    auto probeStoredKeys = keyLayout_.unpack(ctx, probeEntry);
    auto probeKeysMatch = emitKeyComparison(ctx, probeStoredKeys, keys);
    ifProbeNull.done();

    // After the if: result entry and whether we're done.
    auto loopResultEntry = ifProbeNull.phi(insertedEntry, probeEntry);
    auto isDone = ifProbeNull.phi(insertDone, probeKeysMatch);
    auto shouldContinue = ctx.lnot(isDone);

    // If continuing, call probeNext for the next iteration.
    auto nextProbe = ctx.call<Ptr<Int8>>(probeNextFn_, tablePtr, probeEntry, hashVal);
    // Use loopResultEntry if done, nextProbe if continuing.
    auto nextLoopVar = Ptr<Int8>(
        bld.emitSelect(ir::TypeId::PTR, shouldContinue.op(), nextProbe.op(), loopResultEntry.op()));

    loop.done(shouldContinue, {nextLoopVar.op()});

    // After loop, the result is loopResultEntry from the last iteration.
    // But since we exit when done, the loop var at exit equals loopResultEntry.
    // Actually, after loop.done(), we're in the exit block.
    // The value we want is loopResultEntry, which was computed in the loop body.
    // It dominates the exit block (exit is successor of the condbr in the body).

    ifFirstMatch.done();
    auto slowResult = ifFirstMatch.phi(firstMatchEntry, loopResultEntry);

    ifInitNull.done();
    auto finalResult = ifInitNull.phi(fastEntry, slowResult);

    return finalResult;
}

void HashTableCodegen::scan(CodegenContext& ctx, Ptr<Int8> tablePtr, Int64 startSlot, Int64 endSlot,
                            const std::function<void(Ptr<Int8>)>& callback) {
    auto slotBuf = ctx.call<Ptr<Int8>>(getSlotBufferFn_, tablePtr);
    auto stride = ctx.call<UInt64>(getSlotStrideFn_, tablePtr);

    // Cast startSlot/endSlot to UInt64 (matching stride type).
    auto startU = UInt64(ctx.builder().emitBitcast(ir::TypeId::UINT64, startSlot.op()));
    auto endU = UInt64(ctx.builder().emitBitcast(ir::TypeId::UINT64, endSlot.op()));

    // Loop from startSlot to endSlot.
    LoopBuilder loop(ctx, {startU.op()}, {ir::TypeId::UINT64});
    auto loopI = loop.getLoopVar<UInt64>(0);

    // Compute slot pointer: slotBuf + i * stride.
    auto byteOffset = loopI * stride;
    auto slotPtr =
        Ptr<Int8>(ctx.builder().emitGEP(slotBuf.op(), ir::TypeId::INT8, byteOffset.op()));

    // Check occupied flag (first byte of slot).
    auto occupiedPtr = slotPtr.bitcastTo<Int8>(ctx);
    auto occupied = occupiedPtr.load(ctx);
    auto isOccupied =
        Bool(ctx.builder().emitCmpNe(ir::TypeId::INT8, occupied.op(), ctx.constInt8(0).op()));

    IfBuilder ifOcc(ctx, isOccupied);
    {
        // Compute payload pointer: slot + PAYLOAD_OFFSET.
        auto payloadOffset = ctx.constInt64(static_cast<int64_t>(HashTable::PAYLOAD_OFFSET));
        auto payloadPtr =
            Ptr<Int8>(ctx.builder().emitGEP(slotPtr.op(), ir::TypeId::INT8, payloadOffset.op()));
        callback(payloadPtr);
    }
    ifOcc.done();

    auto one = ctx.constUInt64(1);
    auto nextI = loopI + one;
    auto continueLoop = nextI < endU;
    loop.done(continueLoop, {nextI.op()});
}

}  // namespace umbra::codegen
