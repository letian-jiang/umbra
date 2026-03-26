#include "codegen/tuples/tuple_layout.h"

#include <cassert>

namespace umbra::codegen {

// ── Hash constants (§4.4 of tidy-tuples.md) ─────────────────────────────────

static constexpr uint64_t HASH_SEED_1 = 0x9E3779B97F4A7C15ULL;
static constexpr uint64_t HASH_SEED_2 = 0x517CC1B727220A95ULL;
static constexpr uint64_t MIX_PRIME = 0x9FB21C651E98DF25ULL;

// ── Layout computation ──────────────────────────────────────────────────────

uint32_t TupleLayout::alignmentOf(ir::TypeId type) {
    switch (type) {
        case ir::TypeId::VOID:
        case ir::TypeId::BOOL:
        case ir::TypeId::INT8:
            return 1;
        case ir::TypeId::INT32:
            return 4;
        case ir::TypeId::INT64:
        case ir::TypeId::UINT64:
        case ir::TypeId::PTR:
            return 8;
    }
    return 1;
}

uint32_t TupleLayout::alignUp(uint32_t offset, uint32_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

TupleLayout::TupleLayout(const std::vector<SQLType>& types) {
    uint32_t currentOffset = 0;

    fields_.reserve(types.size());
    for (const auto& sqlType : types) {
        auto irType = sqlTypeToIR(sqlType);
        auto alignment = alignmentOf(irType);
        auto size = ir::typeSize(irType);

        currentOffset = alignUp(currentOffset, alignment);

        fields_.push_back({currentOffset, irType, sqlType});
        currentOffset += size;
    }

    // Null bitmap comes after all fields (1-byte aligned).
    nullBitmapOffset_ = currentOffset;
    // One byte per 8 fields, rounded up.
    nullBitmapSize_ = (static_cast<uint32_t>(types.size()) + 7) / 8;
    currentOffset += nullBitmapSize_;

    totalSize_ = currentOffset;
}

// ── Pack ────────────────────────────────────────────────────────────────────

void TupleLayout::pack(CodegenContext& ctx, Ptr<Int8> target,
                       const std::vector<SQLValue>& values) const {
    assert(values.size() == fields_.size());

    auto& bld = ctx.builder();

    // Store each field value.
    for (size_t i = 0; i < values.size(); ++i) {
        auto fieldOffset = ctx.constInt64(static_cast<int64_t>(fields_[i].offset));
        auto fieldPtr = Ptr<Int8>(bld.emitGEP(target.op(), ir::TypeId::INT8, fieldOffset.op()));
        // Bitcast to typed pointer and store.
        auto typedPtr = bld.emitBitcast(ir::TypeId::PTR, fieldPtr.op());
        bld.emitStore(values[i].value(), typedPtr);
    }

    // Pack null bitmap.
    // For MVP simplicity: write one byte per 8 fields.
    for (uint32_t byteIdx = 0; byteIdx < nullBitmapSize_; ++byteIdx) {
        // Build the byte from individual null flags.
        ir::IROp byteval = bld.emitConst(ir::TypeId::INT8, 0);

        for (uint32_t bit = 0; bit < 8; ++bit) {
            auto fieldIdx = byteIdx * 8 + bit;
            if (fieldIdx >= values.size()) {
                break;
            }
            // null_flag is Bool (1 byte). Shift left by bit position and OR into byte.
            auto nullExt = bld.emitZExt(ir::TypeId::INT8, values[fieldIdx].nullFlag());
            auto shiftAmt = bld.emitConst(ir::TypeId::INT8, bit);
            auto shifted = bld.emitShl(ir::TypeId::INT8, nullExt, shiftAmt);
            byteval = bld.emitOr(ir::TypeId::INT8, byteval, shifted);
        }

        auto bitmapOff =
            ctx.constInt64(static_cast<int64_t>(nullBitmapOffset_) + static_cast<int64_t>(byteIdx));
        auto bitmapPtr = Ptr<Int8>(bld.emitGEP(target.op(), ir::TypeId::INT8, bitmapOff.op()));
        bld.emitStore(byteval, bitmapPtr.op());
    }
}

// ── Unpack ──────────────────────────────────────────────────────────────────

std::vector<SQLValue> TupleLayout::unpack(CodegenContext& ctx, Ptr<Int8> source) const {
    auto& bld = ctx.builder();
    std::vector<SQLValue> result;
    result.reserve(fields_.size());

    // First, load the null bitmap bytes.
    std::vector<ir::IROp> bitmapBytes;
    bitmapBytes.reserve(nullBitmapSize_);
    for (uint32_t byteIdx = 0; byteIdx < nullBitmapSize_; ++byteIdx) {
        auto bitmapOffset =
            ctx.constInt64(static_cast<int64_t>(nullBitmapOffset_) + static_cast<int64_t>(byteIdx));
        auto bitmapPtr = Ptr<Int8>(bld.emitGEP(source.op(), ir::TypeId::INT8, bitmapOffset.op()));
        auto byteVal = bld.emitLoad(ir::TypeId::INT8, bitmapPtr.op());
        bitmapBytes.push_back(byteVal);
    }

    // Load each field and extract its null bit.
    for (size_t i = 0; i < fields_.size(); ++i) {
        auto fieldOffset = ctx.constInt64(static_cast<int64_t>(fields_[i].offset));
        auto fieldPtr = Ptr<Int8>(bld.emitGEP(source.op(), ir::TypeId::INT8, fieldOffset.op()));
        auto typedPtr = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, fieldPtr.op()));
        auto value = bld.emitLoad(fields_[i].irType, typedPtr.op());

        // Extract null bit.
        auto byteIdx = static_cast<uint32_t>(i) / 8;
        auto bitIdx = static_cast<uint32_t>(i) % 8;
        auto shiftAmt = bld.emitConst(ir::TypeId::INT8, bitIdx);
        auto shifted = bld.emitLShr(ir::TypeId::INT8, bitmapBytes[byteIdx], shiftAmt);
        auto one = bld.emitConst(ir::TypeId::INT8, 1);
        auto masked = bld.emitAnd(ir::TypeId::INT8, shifted, one);
        auto nullFlag = bld.emitTrunc(ir::TypeId::BOOL, masked);

        result.emplace_back(value, nullFlag, fields_[i].sqlType);
    }

    return result;
}

// ── Hash ────────────────────────────────────────────────────────────────────

UInt64 TupleLayout::hash(CodegenContext& ctx, const std::vector<SQLValue>& keys) {
    auto& bld = ctx.builder();

    auto h1 = ctx.constUInt64(HASH_SEED_1);
    auto h2 = ctx.constUInt64(HASH_SEED_2);

    for (const auto& key : keys) {
        // CHAR(n>1) content-based hashing not yet implemented.
        assert(!(key.type().id == SQLTypeId::CHAR && key.type().maxLen > 1) &&
               "CHAR(n>1) hashing not implemented");
        // Convert key value to UInt64 for CRC32.
        auto irType = sqlTypeToIR(key.type());
        ir::IROp asU64{};
        if (irType == ir::TypeId::INT8 || irType == ir::TypeId::BOOL ||
            irType == ir::TypeId::INT32) {
            // Zero-extend smaller types to UInt64 for hashing.
            asU64 = bld.emitZExt(ir::TypeId::UINT64, key.value());
        } else {
            // Already Int64/UInt64/Ptr — bitcast to UInt64.
            asU64 = bld.emitBitcast(ir::TypeId::UINT64, key.value());
        }

        // Two-way CRC32.
        h1 = UInt64(bld.emitCRC32(asU64, h1.op()));
        h2 = UInt64(bld.emitCRC32(asU64, h2.op()));
    }

    // Combine: hash = (h1 ^ rotr(h2, 32)) * MIX_PRIME
    auto h2Rot =
        UInt64(bld.emitRotr(ir::TypeId::UINT64, h2.op(), bld.emitConst(ir::TypeId::UINT64, 32)));
    auto combined = h1 ^ h2Rot;
    auto prime = ctx.constUInt64(MIX_PRIME);
    return combined * prime;
}

}  // namespace umbra::codegen
