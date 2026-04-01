#include "codegen/operators/projection.h"

#include <cassert>
#include <cstdint>

#include "binder/bound_expr.h"
#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"

namespace umbra::codegen {

ProjectionTranslator::ProjectionTranslator(const optimizer::PhysicalProjection* proj)
    : proj_(proj) {}

void ProjectionTranslator::produce(CompilationContext& ctx) {
    // Projection is a passthrough — just drive the child.
    assert(child_ != nullptr && "Projection must have a child");
    child_->produce(ctx);
}

void ProjectionTranslator::consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) {
    // Evaluate each projection expression.
    std::vector<SQLValue> outputTuple;
    outputTuple.reserve(proj_->expressions.size());

    for (const auto* expr : proj_->expressions) {
        outputTuple.push_back(evaluateProjectionExpr(ctx, expr, tuple));
    }

    parent_->consume(ctx, outputTuple);
}

SQLValue ProjectionTranslator::evaluateProjectionExpr(CompilationContext& ctx,
                                                      const binder::BoundExpr* expr,
                                                      const std::vector<SQLValue>& tuple) {
    auto& cg = ctx.codegen;

    switch (expr->kind) {
        case binder::BoundExprKind::COLUMN_REF: {
            const auto* col = static_cast<const binder::BoundColumnRef*>(expr);
            // For non-aggregate projections, the tuple comes from the scan.
            // Find the position of this column in the scan's column list.
            auto colIndices = getInputColumnIndices();
            for (size_t i = 0; i < colIndices.size(); ++i) {
                if (colIndices[i] == col->columnIndex) {
                    return tuple[i];
                }
            }
            assert(false && "Column not found in scan output");
            return tuple[0];
        }
        case binder::BoundExprKind::GROUP_REF: {
            const auto* ref = static_cast<const binder::BoundGroupRef*>(expr);
            assert(ref->groupIndex < tuple.size());
            return tuple[ref->groupIndex];
        }
        case binder::BoundExprKind::AGG_REF: {
            const auto* ref = static_cast<const binder::BoundAggRef*>(expr);
            // Tuple layout: [group_key_0, ..., group_key_K, agg_0, ..., agg_N].
            // numGroupKeys is set by the optimizer on the PhysicalProjection node.
            auto idx = static_cast<size_t>(ref->aggIndex) + proj_->numGroupKeys;
            assert(idx < tuple.size());
            // The agg state is stored as BIGINT in the hash table,
            // but the projection expression's result type may be different
            // (e.g., DECIMAL for SUM results).
            return {tuple[idx].value(), tuple[idx].nullFlag(), expr->resultType};
        }
        case binder::BoundExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const binder::BoundBinaryExpr*>(expr);
            auto lhs = evaluateProjectionExpr(ctx, bin->left, tuple);
            auto rhs = evaluateProjectionExpr(ctx, bin->right, tuple);

            switch (bin->op) {
                case parser::BinaryOp::ADD:
                    return SQLValue::evaluateBinary(cg, BinaryOp::ADD, lhs, rhs);
                case parser::BinaryOp::SUB:
                    return SQLValue::evaluateBinary(cg, BinaryOp::SUB, lhs, rhs);
                case parser::BinaryOp::MUL:
                    return SQLValue::evaluateBinary(cg, BinaryOp::MUL, lhs, rhs);
                case parser::BinaryOp::DIV:
                    return SQLValue::evaluateBinary(cg, BinaryOp::DIV, lhs, rhs);
                default:
                    assert(false && "Unsupported binary op in projection");
                    return lhs;
            }
        }
        case binder::BoundExprKind::LITERAL: {
            const auto* lit = static_cast<const binder::BoundLiteral*>(expr);
            return SQLValue::makeConst(cg, lit->intValue, lit->resultType);
        }
        case binder::BoundExprKind::CAST: {
            const auto* cast = static_cast<const binder::BoundCast*>(expr);
            auto inner = evaluateProjectionExpr(ctx, cast->expr, tuple);
            return inner.castTo(cg, cast->resultType);
        }
        default:
            assert(false && "Unsupported expression kind in projection");
            return tuple[0];
    }
}

std::vector<uint32_t> ProjectionTranslator::getInputColumnIndices() const {
    // Walk down the physical operator tree to find the table scan.
    auto* current = proj_->child;
    while (current != nullptr) {
        if (current->kind == optimizer::PhysicalNodeKind::TABLE_SCAN) {
            return static_cast<const optimizer::PhysicalTableScan*>(current)->columnIndices;
        }
        switch (current->kind) {
            case optimizer::PhysicalNodeKind::HASH_AGGREGATE:
                current = static_cast<const optimizer::PhysicalHashAggregate*>(current)->child;
                break;
            case optimizer::PhysicalNodeKind::PROJECTION:
                current = static_cast<const optimizer::PhysicalProjection*>(current)->child;
                break;
            case optimizer::PhysicalNodeKind::SORT:
                current = static_cast<const optimizer::PhysicalSort*>(current)->child;
                break;
            default:
                current = nullptr;
                break;
        }
    }
    return {};
}

}  // namespace umbra::codegen
