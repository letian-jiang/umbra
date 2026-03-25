#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "optimizer/physical_operator.h"

namespace umbra::optimizer {

/// Result of optimization: physical plan root + pipeline decomposition.
/// The Optimizer owns all allocated physical operator nodes.
struct OptimizeResult {
    PhysicalOperator* root{nullptr};
    std::vector<Pipeline> pipelines;
};

/// Rule-based optimizer that transforms a bound logical plan into a physical
/// plan with pipeline decomposition.
///
/// Passes (in order):
///   1. Predicate pushdown  — merge filter into scan
///   2. Projection pushdown — prune unused columns from scan
///   3. Physical plan generation — logical -> physical operator mapping
///   4. Pipeline decomposition — identify pipeline breaks
class Optimizer {
   public:
    /// Optimize a bound logical plan, producing a physical plan with pipelines.
    OptimizeResult optimize(const binder::BindResult& bindResult);

    /// Decompose a physical plan into pipelines.
    /// Also used by PipelineExecutor and tests.
    static std::vector<Pipeline> decomposePipelines(PhysicalOperator* root);

   private:
    // ── Node Allocation ─────────────────────────────────────────────────────
    std::vector<std::unique_ptr<PhysicalOperator>> ownedOps_;

    template <typename T, typename... Args>
    T* allocOp(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = ptr.get();
        ownedOps_.push_back(std::move(ptr));
        return raw;
    }

    // ── Pass 2: Projection Pushdown ─────────────────────────────────────────

    /// Collect all column indices referenced by an expression tree.
    static void collectColumnRefs(const binder::BoundExpr* expr, std::set<uint32_t>& columnIndices);

    /// Collect all column indices required by the entire plan.
    static std::set<uint32_t> collectRequiredColumns(const binder::BoundLogicalOperator* op);

    // ── Pass 3: Physical Plan Generation ────────────────────────────────────

    /// Convert logical plan tree to physical plan tree.
    PhysicalOperator* generatePhysicalPlan(const binder::BoundLogicalOperator* op,
                                           const std::set<uint32_t>& requiredColumns);
};

}  // namespace umbra::optimizer
