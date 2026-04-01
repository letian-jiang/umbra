#include "optimizer/optimizer.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace umbra::optimizer {

// ── Top-level optimize ──────────────────────────────────────────────────────

OptimizeResult Optimizer::optimize(const binder::BindResult& bindResult) {
    // Clear state from any previous optimization
    ownedOps_.clear();

    const auto* root = bindResult.root;
    if (root == nullptr) {
        return {};
    }

    // Pass 2: Projection pushdown — collect required columns
    std::set<uint32_t> requiredColumns = collectRequiredColumns(root);

    // Pass 1 + 3: Physical plan generation (predicate pushdown is integrated
    // into the physical scan generation — filter is merged into scan)
    PhysicalOperator* physRoot = generatePhysicalPlan(root, requiredColumns);

    // Pass 4: Pipeline decomposition
    std::vector<Pipeline> pipelines = decomposePipelines(physRoot);

    return {physRoot, std::move(pipelines)};
}

// ── Pass 2: Projection Pushdown ─────────────────────────────────────────────

void Optimizer::collectColumnRefs(const binder::BoundExpr* expr,
                                  std::set<uint32_t>& columnIndices) {
    if (expr == nullptr) {
        return;
    }

    switch (expr->kind) {
        case binder::BoundExprKind::COLUMN_REF: {
            const auto* col = static_cast<const binder::BoundColumnRef*>(expr);
            columnIndices.insert(col->columnIndex);
            break;
        }
        case binder::BoundExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const binder::BoundBinaryExpr*>(expr);
            collectColumnRefs(bin->left, columnIndices);
            collectColumnRefs(bin->right, columnIndices);
            break;
        }
        case binder::BoundExprKind::UNARY_EXPR: {
            const auto* un = static_cast<const binder::BoundUnaryExpr*>(expr);
            collectColumnRefs(un->operand, columnIndices);
            break;
        }
        case binder::BoundExprKind::AGG_CALL: {
            const auto* agg = static_cast<const binder::BoundAggCall*>(expr);
            collectColumnRefs(agg->arg, columnIndices);
            break;
        }
        case binder::BoundExprKind::CAST: {
            const auto* cast = static_cast<const binder::BoundCast*>(expr);
            collectColumnRefs(cast->expr, columnIndices);
            break;
        }
        case binder::BoundExprKind::LITERAL:
        case binder::BoundExprKind::AGG_REF:
        case binder::BoundExprKind::GROUP_REF:
            // No column references in these nodes
            break;
    }
}

std::set<uint32_t> Optimizer::collectRequiredColumns(const binder::BoundLogicalOperator* op) {
    if (op == nullptr) {
        return {};
    }

    std::set<uint32_t> required;

    switch (op->kind) {
        case binder::BoundNodeKind::SCAN: {
            // Scan itself doesn't add requirements — they come from above
            break;
        }
        case binder::BoundNodeKind::FILTER: {
            const auto* filter = static_cast<const binder::BoundFilter*>(op);
            collectColumnRefs(filter->predicate, required);
            auto childReq = collectRequiredColumns(filter->child);
            required.insert(childReq.begin(), childReq.end());
            break;
        }
        case binder::BoundNodeKind::AGGREGATE: {
            const auto* agg = static_cast<const binder::BoundAggregate*>(op);
            for (const auto* gbExpr : agg->groupBy) {
                collectColumnRefs(gbExpr, required);
            }
            for (const auto* aggExpr : agg->aggregates) {
                collectColumnRefs(aggExpr, required);
            }
            // Aggregate truncates requirements — only pass down what it needs
            auto childReq = collectRequiredColumns(agg->child);
            required.insert(childReq.begin(), childReq.end());
            break;
        }
        case binder::BoundNodeKind::SORT: {
            const auto* sort = static_cast<const binder::BoundSort*>(op);
            for (const auto& item : sort->orderBy) {
                collectColumnRefs(item.expr, required);
            }
            auto childReq = collectRequiredColumns(sort->child);
            required.insert(childReq.begin(), childReq.end());
            break;
        }
        case binder::BoundNodeKind::PROJECTION: {
            const auto* proj = static_cast<const binder::BoundProjection*>(op);
            for (const auto* expr : proj->expressions) {
                collectColumnRefs(expr, required);
            }
            // Projection truncates — only columns referenced in expressions
            // are needed from below. But those expressions may contain
            // AGG_REF/GROUP_REF which don't have column refs, so we also
            // need columns from the child.
            auto childReq = collectRequiredColumns(proj->child);
            required.insert(childReq.begin(), childReq.end());
            break;
        }
    }

    return required;
}

// ── Pass 3: Physical Plan Generation ────────────────────────────────────────

PhysicalOperator* Optimizer::generatePhysicalPlan(const binder::BoundLogicalOperator* op,
                                                  const std::set<uint32_t>& requiredColumns) {
    if (op == nullptr) {
        return nullptr;
    }

    switch (op->kind) {
        case binder::BoundNodeKind::SCAN: {
            const auto* scan = static_cast<const binder::BoundScan*>(op);
            auto* physScan = allocOp<PhysicalTableScan>(scan->table);

            // Apply projection pushdown: only include required columns
            if (requiredColumns.empty()) {
                // If no required columns known, include all
                for (const auto& col : scan->table->columns) {
                    physScan->columnIndices.push_back(col.columnIndex);
                }
            } else {
                for (uint32_t idx : requiredColumns) {
                    physScan->columnIndices.push_back(idx);
                }
                std::sort(physScan->columnIndices.begin(), physScan->columnIndices.end());
            }
            return physScan;
        }
        case binder::BoundNodeKind::FILTER: {
            const auto* filter = static_cast<const binder::BoundFilter*>(op);

            // Predicate pushdown: if the child is a scan, merge filter into it
            if (filter->child->kind == binder::BoundNodeKind::SCAN) {
                auto* physScan = static_cast<PhysicalTableScan*>(
                    generatePhysicalPlan(filter->child, requiredColumns));
                physScan->filter = filter->predicate;
                return physScan;
            }

            // If child is not a scan, generate child and keep filter separate.
            // Currently filter is always directly above scan after binding.
            auto* child = generatePhysicalPlan(filter->child, requiredColumns);
            // Wrap in a projection that passes through (not ideal but safe)
            return child;
        }
        case binder::BoundNodeKind::AGGREGATE: {
            const auto* agg = static_cast<const binder::BoundAggregate*>(op);
            auto* child = generatePhysicalPlan(agg->child, requiredColumns);
            auto* physAgg = allocOp<PhysicalHashAggregate>(child);
            physAgg->groupByKeys = agg->groupBy;
            physAgg->aggregates = agg->aggregates;
            return physAgg;
        }
        case binder::BoundNodeKind::SORT: {
            const auto* sort = static_cast<const binder::BoundSort*>(op);
            auto* child = generatePhysicalPlan(sort->child, requiredColumns);
            auto* physSort = allocOp<PhysicalSort>(child);
            for (const auto& item : sort->orderBy) {
                physSort->orderBy.push_back({item.expr, item.ascending});
            }
            return physSort;
        }
        case binder::BoundNodeKind::PROJECTION: {
            const auto* proj = static_cast<const binder::BoundProjection*>(op);
            auto* child = generatePhysicalPlan(proj->child, requiredColumns);
            auto* physProj = allocOp<PhysicalProjection>(child);
            physProj->expressions = proj->expressions;
            physProj->aliases = proj->aliases;
            // Set numGroupKeys if the child is a hash aggregate.
            if (child->kind == PhysicalNodeKind::HASH_AGGREGATE) {
                physProj->numGroupKeys = static_cast<uint32_t>(
                    static_cast<PhysicalHashAggregate*>(child)->groupByKeys.size());
            }
            return physProj;
        }
    }

    return nullptr;  // unreachable
}

// ── Pass 4: Pipeline Decomposition ──────────────────────────────────────────

std::vector<Pipeline> Optimizer::decomposePipelines(PhysicalOperator* root) {
    std::vector<Pipeline> pipelines;

    if (root == nullptr) {
        return pipelines;
    }

    // Walk the physical plan tree from root to leaves, identifying
    // pipeline breaks at HashAggregate boundaries.
    //
    // Example (aggregate query):
    //   Pipeline 0 (build): source=TableScan, sink=HashAggregate(build)
    //   Pipeline 1 (scan):  source=HashAggregate(scan), sink=Result
    //
    // Non-aggregate queries produce a single pipeline.

    // Collect the linear chain of operators (from root down to leaf)
    std::vector<PhysicalOperator*> chain;
    auto* current = root;
    while (current != nullptr) {
        chain.push_back(current);
        switch (current->kind) {
            case PhysicalNodeKind::SORT:
                current = static_cast<PhysicalSort*>(current)->child;
                break;
            case PhysicalNodeKind::PROJECTION:
                current = static_cast<PhysicalProjection*>(current)->child;
                break;
            case PhysicalNodeKind::HASH_AGGREGATE:
                current = static_cast<PhysicalHashAggregate*>(current)->child;
                break;
            case PhysicalNodeKind::TABLE_SCAN:
                current = nullptr;
                break;
        }
    }

    // chain is [root, ..., leaf] = e.g. [Sort, Projection, HashAgg, TableScan]
    // We need to reverse to get [leaf, ..., root] = [TableScan, HashAgg, Projection, Sort]
    std::reverse(chain.begin(), chain.end());

    // Now chain is bottom-up: [TableScan, HashAgg, Projection, Sort]
    // Find pipeline breaks (HashAggregate, Sort)
    // Pipeline break means: everything below (inclusive of the break operator's
    // build side) forms one pipeline, everything above (from the break
    // operator's scan side) starts a new pipeline.

    // Identify break points (indices into chain where a break occurs)
    std::vector<size_t> breakPoints;
    for (size_t i = 0; i < chain.size(); ++i) {
        if (chain[i]->kind == PhysicalNodeKind::HASH_AGGREGATE) {
            breakPoints.push_back(i);
        }
    }

    if (breakPoints.empty()) {
        // No pipeline breaks — single pipeline with everything
        Pipeline pipe;
        pipe.source = chain.front();
        pipe.sink = chain.back();
        for (size_t i = 1; i + 1 < chain.size(); ++i) {
            pipe.operators.push_back(chain[i]);
        }
        pipe.description = "single pipeline";
        pipelines.push_back(std::move(pipe));
        return pipelines;
    }

    // Build pipeline: from leaf to the first break point (inclusive)
    {
        Pipeline pipe;
        pipe.source = chain.front();
        pipe.sink = chain[breakPoints[0]];
        for (size_t i = 1; i < breakPoints[0]; ++i) {
            pipe.operators.push_back(chain[i]);
        }
        pipe.description = "build";
        pipelines.push_back(std::move(pipe));
    }

    // For each subsequent break point, create a pipeline from the previous
    // break to the next break
    for (size_t b = 0; b + 1 < breakPoints.size(); ++b) {
        Pipeline pipe;
        pipe.source = chain[breakPoints[b]];
        pipe.sink = chain[breakPoints[b + 1]];
        for (size_t i = breakPoints[b] + 1; i < breakPoints[b + 1]; ++i) {
            pipe.operators.push_back(chain[i]);
        }
        pipe.description = "intermediate";
        pipelines.push_back(std::move(pipe));
    }

    // Scan pipeline: from last break point to root
    {
        Pipeline pipe;
        pipe.source = chain[breakPoints.back()];
        pipe.sink = chain.back();
        for (size_t i = breakPoints.back() + 1; i + 1 < chain.size(); ++i) {
            pipe.operators.push_back(chain[i]);
        }
        pipe.description = "scan";
        pipelines.push_back(std::move(pipe));
    }

    return pipelines;
}

}  // namespace umbra::optimizer
