#pragma once

#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// ProjectionTranslator — evaluates output expressions.
///
/// For aggregate queries, the input tuple is [group_keys..., agg_states...].
/// The projection expressions reference group keys via BoundGroupRef
/// and aggregate results via BoundAggRef. numGroupKeys on the physical
/// operator determines the boundary between keys and aggregates.
class ProjectionTranslator : public OperatorTranslator {
   public:
    explicit ProjectionTranslator(const optimizer::PhysicalProjection* proj);

    void produce(CompilationContext& ctx) override;

    /// Consume a tuple from the child, evaluate projection expressions,
    /// and pass the result to the parent.
    void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) override;

    /// Set the child translator.
    void setChild(OperatorTranslator* child) { child_ = child; }

    /// Get the physical operator.
    [[nodiscard]] const optimizer::PhysicalProjection* physicalOp() const { return proj_; }

   private:
    const optimizer::PhysicalProjection* proj_;
    OperatorTranslator* child_{nullptr};

    /// Evaluate a projection expression.
    SQLValue evaluateProjectionExpr(CompilationContext& ctx, const binder::BoundExpr* expr,
                                    const std::vector<SQLValue>& tuple);

    /// Get column indices from the child table scan (for non-aggregate projections).
    [[nodiscard]] std::vector<uint32_t> getInputColumnIndices() const;
};

}  // namespace umbra::codegen
