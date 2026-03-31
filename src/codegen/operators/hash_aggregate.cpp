#include "codegen/operators/hash_aggregate.h"

#include <cassert>
#include <cstdint>

#include "binder/bound_expr.h"
#include "codegen/api/codegen_context.h"
#include "codegen/operators/table_scan.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/structures/hash_table.h"
#include "codegen/tuples/tuple_layout.h"
#include "common/types.h"
#include "ir/opcode.h"

namespace umbra::codegen {

HashAggregateTranslator::HashAggregateTranslator(const optimizer::PhysicalHashAggregate* agg)
    : agg_(agg) {
    // Compute key types from group-by expressions.
    for (const auto* keyExpr : agg_->groupByKeys) {
        keyTypes_.push_back(keyExpr->resultType);
    }

    // Each aggregate state is stored as Int64 (for SUM and COUNT).
    for (size_t i = 0; i < agg_->aggregates.size(); ++i) {
        aggStateTypes_.push_back(SQLType::bigint());
    }

    // Create the hash table codegen interface.
    htCodegen_ = std::make_unique<HashTableCodegen>(keyTypes_, aggStateTypes_);

    // Create the runtime hash table with the computed entry size.
    hashTable_ = std::make_unique<HashTable>(htCodegen_->entrySize());
}

void HashAggregateTranslator::produce(CompilationContext& ctx) {
    // Build side: register host functions and drive child.
    htCodegen_->registerHostFunctions(ctx.codegen);

    // Drive child to produce tuples.
    assert(child_ != nullptr && "HashAggregate must have a child");
    child_->produce(ctx);
}

void HashAggregateTranslator::consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) {
    auto& cg = ctx.codegen;
    auto& bld = cg.builder();

    // Get column indices from the input (via the table scan).
    auto columnIndices = getInputColumnIndices();

    // Extract group-by key values from the tuple.
    std::vector<SQLValue> keys;
    keys.reserve(agg_->groupByKeys.size());
    for (const auto* keyExpr : agg_->groupByKeys) {
        keys.push_back(evaluateAggInputExpr(ctx, keyExpr, tuple, columnIndices));
    }

    // Compute hash of group-by keys.
    auto hashVal = TupleLayout::hash(cg, keys);

    // Load hash table pointer from RuntimeContext (offset 24).
    auto htOffset = cg.constInt64(24);
    auto htFieldPtr = Ptr<Int8>(bld.emitGEP(ctx.runtimeCtx.op(), ir::TypeId::INT8, htOffset.op()));
    auto htFieldPtrTyped = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, htFieldPtr.op()));
    auto htPtr = Ptr<Int8>(bld.emitLoad(ir::TypeId::PTR, htFieldPtrTyped.op()));

    // Find or insert entry.
    auto entryPtr = htCodegen_->findOrInsert(cg, htPtr, hashVal, keys);

    // Compute aggregate state offset within the entry.
    auto aggOff = cg.constInt64(static_cast<int64_t>(htCodegen_->aggOffset()));
    auto stateBase = Ptr<Int8>(bld.emitGEP(entryPtr.op(), ir::TypeId::INT8, aggOff.op()));

    // Update each aggregate state.
    const auto& aggLayout = htCodegen_->aggLayout();

    for (size_t i = 0; i < agg_->aggregates.size(); ++i) {
        const auto* aggCall = agg_->aggregates[i];
        auto fieldOffset = cg.constInt64(static_cast<int64_t>(aggLayout.field(i).offset));
        auto slotPtr = Ptr<Int8>(bld.emitGEP(stateBase.op(), ir::TypeId::INT8, fieldOffset.op()));
        auto typedSlotPtr = Ptr<Int64>(bld.emitBitcast(ir::TypeId::PTR, slotPtr.op()));

        auto oldVal = typedSlotPtr.load(cg);

        switch (aggCall->func) {
            case binder::AggFunc::SUM: {
                // Evaluate the aggregate input expression.
                auto inputVal = evaluateAggInputExpr(ctx, aggCall->arg, tuple, columnIndices);
                // Cast input to Int64 if needed.
                auto inputAsI64 = inputVal.castTo(cg, SQLType::bigint());
                auto newVal = oldVal + Int64(inputAsI64.value());
                typedSlotPtr.store(cg, newVal);
                break;
            }
            case binder::AggFunc::COUNT: {
                auto one = cg.constInt64(1);
                auto newVal = oldVal + one;
                typedSlotPtr.store(cg, newVal);
                break;
            }
        }
    }
}

void HashAggregateTranslator::produceScan(CompilationContext& ctx) {
    auto& cg = ctx.codegen;
    auto& bld = cg.builder();

    // Register host functions (for scan-side pipeline, which has its own codegen context).
    htCodegen_->registerHostFunctions(cg);

    // Load hash table pointer from RuntimeContext (offset 24).
    auto htOffset = cg.constInt64(24);
    auto htFieldPtr = Ptr<Int8>(bld.emitGEP(ctx.runtimeCtx.op(), ir::TypeId::INT8, htOffset.op()));
    auto htFieldPtrTyped = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, htFieldPtr.op()));
    auto htPtr = Ptr<Int8>(bld.emitLoad(ir::TypeId::PTR, htFieldPtrTyped.op()));

    // Scan entries in the hash table using morsel bounds.
    htCodegen_->scan(cg, htPtr, ctx.startRow, ctx.endRow, [&](Ptr<Int8> entryPtr) {
        // Unpack group-by keys.
        auto keys = htCodegen_->keyLayout().unpack(cg, entryPtr);

        // Unpack aggregate states.
        auto aggOff = cg.constInt64(static_cast<int64_t>(htCodegen_->aggOffset()));
        auto stateBase = Ptr<Int8>(bld.emitGEP(entryPtr.op(), ir::TypeId::INT8, aggOff.op()));
        auto aggStates = htCodegen_->aggLayout().unpack(cg, stateBase);

        // Build the output tuple: keys + aggregate results.
        // The parent (projection) expects [group(0), group(1), agg(0), ..., agg(N-1)].
        std::vector<SQLValue> outputTuple;
        outputTuple.reserve(keys.size() + aggStates.size());

        // Add group-by keys with their original types.
        for (size_t i = 0; i < keys.size(); ++i) {
            outputTuple.emplace_back(keys[i].value(), keys[i].nullFlag(), keyTypes_[i]);
        }

        // Add aggregate states (as BIGINT — projection will do final computation).
        for (const auto& aggState : aggStates) {
            outputTuple.emplace_back(aggState.value(), aggState.nullFlag(), SQLType::bigint());
        }

        parent_->consume(ctx, outputTuple);
    });
}

SQLValue HashAggregateTranslator::evaluateAggInputExpr(CompilationContext& ctx,
                                                       const binder::BoundExpr* expr,
                                                       const std::vector<SQLValue>& tuple,
                                                       const std::vector<uint32_t>& columnIndices) {
    // Reuse the same expression evaluation logic as TableScan.
    return TableScanTranslator::evaluateExpr(ctx, expr, tuple, columnIndices);
}

std::vector<uint32_t> HashAggregateTranslator::getInputColumnIndices() const {
    // Walk down the physical operator tree to find the table scan.
    auto* current = agg_->child;
    while (current != nullptr) {
        if (current->kind == optimizer::PhysicalNodeKind::TABLE_SCAN) {
            return static_cast<const optimizer::PhysicalTableScan*>(current)->columnIndices;
        }
        // Walk through other operators.
        switch (current->kind) {
            case optimizer::PhysicalNodeKind::PROJECTION:
                current = static_cast<const optimizer::PhysicalProjection*>(current)->child;
                break;
            case optimizer::PhysicalNodeKind::SORT:
                current = static_cast<const optimizer::PhysicalSort*>(current)->child;
                break;
            case optimizer::PhysicalNodeKind::HASH_AGGREGATE:
                current = static_cast<const optimizer::PhysicalHashAggregate*>(current)->child;
                break;
            default:
                current = nullptr;
                break;
        }
    }
    return {};
}

}  // namespace umbra::codegen
