#pragma once

#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// ProjectionTranslator — evaluates output expressions.
///
/// For Q1, the projection computes final AVG = SUM/COUNT and passes
/// through group-by keys and other aggregated values.
/// Input tuple format: [group_key_0, group_key_1, agg_0, agg_1, ..., agg_N]
/// The projection expressions reference group keys via BoundGroupRef
/// and aggregate results via BoundAggRef.
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
    /// The input tuple contains [group_keys..., agg_states...].
    static SQLValue evaluateProjectionExpr(CompilationContext& ctx, const binder::BoundExpr* expr,
                                           const std::vector<SQLValue>& tuple);
};

}  // namespace umbra::codegen
