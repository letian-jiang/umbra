#pragma once

#include <cstdint>
#include <vector>

#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"

namespace umbra::codegen {

/// Field descriptor within a tuple layout.
struct FieldInfo {
    uint32_t offset;    /// Byte offset from tuple start.
    ir::TypeId irType;  /// IR type for load/store.
    SQLType sqlType;    /// Full SQL type info.
};

/// TupleLayout — Layer 3 of Tidy Tuples.
/// Computes field offsets with alignment for a given set of SQL types.
/// Provides codegen methods to pack/unpack SQLValues to/from memory buffers,
/// and to hash group-by keys using CRC32.
class TupleLayout {
   public:
    /// Construct a layout for the given SQL types.
    explicit TupleLayout(const std::vector<SQLType>& types);

    /// Get the total byte size of a packed tuple (including null bitmap).
    [[nodiscard]] uint32_t totalSize() const { return totalSize_; }

    /// Get the number of fields.
    [[nodiscard]] size_t numFields() const { return fields_.size(); }

    /// Get field info by index.
    [[nodiscard]] const FieldInfo& field(size_t index) const { return fields_[index]; }

    /// Get the offset of the null bitmap.
    [[nodiscard]] uint32_t nullBitmapOffset() const { return nullBitmapOffset_; }

    /// Get the byte size of the null bitmap.
    [[nodiscard]] uint32_t nullBitmapSize() const { return nullBitmapSize_; }

    /// Get all field descriptors.
    [[nodiscard]] const std::vector<FieldInfo>& fields() const { return fields_; }

    // ── Codegen methods (emit IR) ────────────────────────────────────────

    /// Pack: write a list of SQLValues into a memory buffer.
    /// Emits IR store instructions for each field and the null bitmap.
    void pack(CodegenContext& ctx, Ptr<Int8> target, const std::vector<SQLValue>& values) const;

    /// Unpack: read SQLValues back from a memory buffer.
    /// Emits IR load instructions for each field and the null bitmap.
    [[nodiscard]] std::vector<SQLValue> unpack(CodegenContext& ctx, Ptr<Int8> source) const;

    /// Hash: compute a CRC32-based hash over a set of SQLValues (for group-by keys).
    /// Returns a UInt64 hash value.
    static UInt64 hash(CodegenContext& ctx, const std::vector<SQLValue>& keys);

   private:
    std::vector<FieldInfo> fields_;
    uint32_t nullBitmapOffset_ = 0;
    uint32_t nullBitmapSize_ = 0;
    uint32_t totalSize_ = 0;

    /// Compute alignment for an IR type.
    static uint32_t alignmentOf(ir::TypeId type);

    /// Align an offset up to the given alignment.
    static uint32_t alignUp(uint32_t offset, uint32_t alignment);
};

}  // namespace umbra::codegen
