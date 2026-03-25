#pragma once

#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// SortTranslator — materializes tuples and sorts via host function.
///
/// For Q1, the result set is tiny (~4 rows), so we materialize all rows
/// into a buffer during consume, then in a finalization step sort them
/// using a host qsort call and emit them to the parent.
///
/// MVP simplification: Sort is handled as a pipeline-breaking operator
/// in the general case, but for Q1 it's part of Pipeline 1 (scan side)
/// and we do the sort inline since there are only ~4 rows.
/// We materialize into the result buffer directly and sort at the end.
class SortTranslator : public OperatorTranslator {
   public:
    explicit SortTranslator(const optimizer::PhysicalSort* sort);

    void produce(CompilationContext& ctx) override;
    void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) override;

    /// Set the child translator.
    void setChild(OperatorTranslator* child) { child_ = child; }

    /// Get the physical operator.
    [[nodiscard]] const optimizer::PhysicalSort* physicalOp() const { return sort_; }

   private:
    const optimizer::PhysicalSort* sort_;
    OperatorTranslator* child_{nullptr};
};

}  // namespace umbra::codegen
