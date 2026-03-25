#pragma once

#include <cstdint>
#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// TableScanTranslator — generates code to scan a table column-by-column.
///
/// Emits a loop over all rows, loading only the pruned columns.
/// If a filter predicate is present (pushed down), it is evaluated
/// inside the loop and only passing rows are sent to the parent.
class TableScanTranslator : public OperatorTranslator {
   public:
    explicit TableScanTranslator(const optimizer::PhysicalTableScan* scan);

    void produce(CompilationContext& ctx) override;
    void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) override;

    /// Get the physical table scan operator.
    [[nodiscard]] const optimizer::PhysicalTableScan* physicalOp() const { return scan_; }

    /// Evaluate a bound expression given the current tuple of loaded column values.
    /// The columnIndices maps positions in the tuple to original column indices.
    /// Also used by HashAggregateTranslator for evaluating aggregate input expressions.
    static SQLValue evaluateExpr(CompilationContext& ctx, const binder::BoundExpr* expr,
                                 const std::vector<SQLValue>& tuple,
                                 const std::vector<uint32_t>& columnIndices);

   private:
    const optimizer::PhysicalTableScan* scan_;
};

}  // namespace umbra::codegen
