#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/structures/hash_table.h"
#include "codegen/tuples/tuple_layout.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// HashAggregateTranslator — generates code for hash-based aggregation.
///
/// Has two modes:
/// - Build side (Pipeline 0): consumes tuples from child, hashes group-by keys,
///   does find-or-insert into hash table, updates aggregate states (SUM, COUNT).
/// - Scan side (Pipeline 1): iterates hash table entries, unpacks keys and states,
///   finalizes aggregates (AVG = SUM/COUNT), produces output tuples.
class HashAggregateTranslator : public OperatorTranslator {
   public:
    explicit HashAggregateTranslator(const optimizer::PhysicalHashAggregate* agg);

    /// Build-side produce: drives child to produce tuples.
    void produce(CompilationContext& ctx) override;

    /// Build-side consume: hash + find-or-insert + update aggregates.
    void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) override;

    /// Scan-side produce: iterate hash table, unpack, finalize, send to parent.
    void produceScan(CompilationContext& ctx);

    /// Get the physical operator.
    [[nodiscard]] const optimizer::PhysicalHashAggregate* physicalOp() const { return agg_; }

    /// Set the child translator (for build side).
    void setChild(OperatorTranslator* child) { child_ = child; }

    /// Get the runtime hash table (created during build-side code generation).
    [[nodiscard]] HashTable& hashTable() { return *hashTable_; }

    /// Get the hash table pointer (for RuntimeContext linking).
    [[nodiscard]] HashTable* hashTablePtr() const { return hashTable_.get(); }

   private:
    const optimizer::PhysicalHashAggregate* agg_;
    OperatorTranslator* child_{nullptr};

    /// Runtime hash table, owned by this translator and shared across pipelines.
    std::unique_ptr<HashTable> hashTable_;

    /// Codegen interface for the hash table.
    std::unique_ptr<HashTableCodegen> htCodegen_;

    /// SQL types for group-by keys.
    std::vector<SQLType> keyTypes_;

    /// SQL types for aggregate states (all as Int64 for SUM/COUNT).
    std::vector<SQLType> aggStateTypes_;

    /// Evaluate a bound expression using the input tuple and column indices
    /// (reuses the same logic as table_scan for expression evaluation).
    static SQLValue evaluateAggInputExpr(CompilationContext& ctx, const binder::BoundExpr* expr,
                                         const std::vector<SQLValue>& tuple,
                                         const std::vector<uint32_t>& columnIndices);

    /// Determine which original column indices are available in the input tuple.
    /// Walks up the operator tree to find the TableScan's column indices.
    [[nodiscard]] std::vector<uint32_t> getInputColumnIndices() const;
};

}  // namespace umbra::codegen
