#include "codegen/operators/result_writer.h"

#include <cassert>
#include <cstdint>

#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"
#include "ir/opcode.h"

namespace umbra::codegen {

ResultWriterTranslator::ResultWriterTranslator(uint32_t numOutputCols)
    : numOutputCols_(numOutputCols) {}

void ResultWriterTranslator::produce(CompilationContext& ctx) {
    // Drive the child to produce tuples.
    assert(child_ != nullptr && "ResultWriter must have a child");
    child_->produce(ctx);
}

void ResultWriterTranslator::consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) {
    auto& cg = ctx.codegen;
    auto& bld = cg.builder();

    // Load the result buffer pointer from RuntimeContext (offset 32).
    auto bufOffset = cg.constInt64(32);
    auto bufFieldPtr =
        Ptr<Int8>(bld.emitGEP(ctx.runtimeCtx.op(), ir::TypeId::INT8, bufOffset.op()));
    auto bufFieldPtrTyped = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, bufFieldPtr.op()));
    auto bufPtr = Ptr<Int64>(bld.emitLoad(ir::TypeId::PTR, bufFieldPtrTyped.op()));

    // Load the current row count from RuntimeContext (offset 40).
    auto rcOffset = cg.constInt64(40);
    auto rcFieldPtr = Ptr<Int8>(bld.emitGEP(ctx.runtimeCtx.op(), ir::TypeId::INT8, rcOffset.op()));
    auto rcFieldPtrTyped = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, rcFieldPtr.op()));
    auto rowCount = Int64(bld.emitLoad(ir::TypeId::INT64, rcFieldPtrTyped.op()));

    // Write each value to buffer[rowCount * numCols + col].
    auto numCols = cg.constInt64(static_cast<int64_t>(numOutputCols_));
    auto baseIdx = rowCount * numCols;

    for (size_t c = 0; c < tuple.size() && c < numOutputCols_; ++c) {
        auto colIdx = cg.constInt64(static_cast<int64_t>(c));
        auto writeIdx = baseIdx + colIdx;
        auto elemPtr = bufPtr.gep(cg, writeIdx);

        // Cast value to Int64 for storage.
        auto irType = sqlTypeToIR(tuple[c].type());
        ir::IROp valAsI64{};
        if (irType == ir::TypeId::INT8 || irType == ir::TypeId::BOOL ||
            irType == ir::TypeId::INT32) {
            // Sign-extend narrow types to Int64.
            valAsI64 = bld.emitSExt(ir::TypeId::INT64, tuple[c].value());
        } else {
            // Already Int64, UInt64, or Ptr — bitcast to Int64.
            valAsI64 = bld.emitBitcast(ir::TypeId::INT64, tuple[c].value());
        }
        elemPtr.store(cg, Int64(valAsI64));
    }

    // Increment row count and store back.
    auto one = cg.constInt64(1);
    auto newRowCount = rowCount + one;
    // Store back to RuntimeContext.
    auto rcStorePtr = Ptr<Int64>(bld.emitBitcast(ir::TypeId::PTR, rcFieldPtr.op()));
    rcStorePtr.store(cg, newRowCount);
}

}  // namespace umbra::codegen
