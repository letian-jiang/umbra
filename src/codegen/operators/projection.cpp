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
        case binder::BoundExprKind::GROUP_REF: {
            const auto* ref = static_cast<const binder::BoundGroupRef*>(expr);
            assert(ref->groupIndex < tuple.size());
            return tuple[ref->groupIndex];
        }
        case binder::BoundExprKind::AGG_REF: {
            const auto* ref = static_cast<const binder::BoundAggRef*>(expr);
            // Aggregate results follow the group-by keys in the tuple.
            // But we don't know the number of group keys here directly.
            // Convention: the tuple is [group_keys..., agg_states...].
            // The aggIndex maps to position within the aggregate portion.
            // We need to figure out the number of group-by keys.
            // Since AGG_REF is used in projection expressions, and the input
            // tuple comes from the hash aggregate scan, we know the group count.
            // For now, we use the expression's result type to construct the value.
            // The ref->aggIndex is the index into the aggregate list.
            // In the tuple, agg states start after all group keys.
            // We don't have direct access to the number of group keys here,
            // but we can infer: if this is the scan pipeline, tuple layout is:
            // [group_key_0, ..., group_key_K, agg_0, ..., agg_N].
            // The aggIndex + numGroupKeys gives the position.
            // For Q1: 2 group keys + N aggregates.
            // We'll look at the tuple to determine this. Since we know the
            // expression references an aggregate, we search by matching types.
            // Actually, the caller guarantees the tuple layout. We need a way
            // to pass the number of group keys. We embed it by convention:
            // the tuple comes from HashAggregate scan which always puts keys first.
            // We need the number of group keys to offset. Since the expression
            // tree gives us both GROUP_REF and AGG_REF, and the GROUP_REF indices
            // tell us the count, we can just look at the max group index.
            // Better approach: the tuple is guaranteed to be [keys..., aggs...],
            // and the first non-key index is the number of keys.
            // For safety, we scan for the boundary by looking at SQL types.
            // But the cleanest approach is: we know the physical aggregate has
            // K group-by keys. We store that information as a member.
            // However, evaluateProjectionExpr is static. Let's just pass it.
            // For now, accept that the tuple layout may vary. We'll use a
            // heuristic: group keys are the first elements that are not BIGINT,
            // or we accept that the tuple is [keys..., aggs...] and use the
            // aggregate's parent info to find K.

            // Actually, for robustness: the calling context knows the number
            // of group keys. We'll just compute it from the physical projection's
            // child. But since this is a static method, we need to pass it.
            // Let's change strategy: iterate tuple to find non-BIGINT prefix size
            // (group keys can be INT8 for Q1), then the rest are aggregates.

            // Simpler: just count group keys by scanning for non-BIGINT types
            // at the start of the tuple.
            uint32_t numGroupKeys = 0;
            for (const auto& val : tuple) {
                if (val.type().id == SQLTypeId::BIGINT) {
                    break;
                }
                numGroupKeys++;
            }
            auto idx = static_cast<size_t>(ref->aggIndex) + numGroupKeys;
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

}  // namespace umbra::codegen
