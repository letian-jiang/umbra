#include "codegen/operators/table_scan.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

#include "binder/bound_expr.h"
#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"
#include "ir/opcode.h"

namespace umbra::codegen {

TableScanTranslator::TableScanTranslator(const optimizer::PhysicalTableScan* scan) : scan_(scan) {}

void TableScanTranslator::produce(CompilationContext& ctx) {
    auto& cg = ctx.codegen;
    auto& bld = cg.builder();

    // Load RuntimeContext fields.
    // RuntimeContext layout:
    //   void** columnPtrs     (offset 0, 8 bytes)
    //   uint64_t rowCount     (offset 8, 8 bytes)
    //   const char* stringBase (offset 16, 8 bytes)
    //   void* hashTable       (offset 24, 8 bytes)
    //   void* resultBuffer    (offset 32, 8 bytes)
    //   uint64_t resultRowCount (offset 40, 8 bytes)

    // Load columnPtrs array pointer from RuntimeContext.
    auto colPtrsFieldPtr = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, ctx.runtimeCtx.op()));
    auto colPtrsBase = Ptr<Int8>(bld.emitLoad(ir::TypeId::PTR, colPtrsFieldPtr.op()));

    // Load rowCount from RuntimeContext (offset 8).
    auto rowCountOffset = cg.constInt64(8);
    auto rowCountPtr =
        Ptr<Int8>(bld.emitGEP(ctx.runtimeCtx.op(), ir::TypeId::INT8, rowCountOffset.op()));
    auto rowCountPtrTyped = Ptr<Int8>(bld.emitBitcast(ir::TypeId::PTR, rowCountPtr.op()));
    auto rowCount = Int64(bld.emitLoad(ir::TypeId::INT64, rowCountPtrTyped.op()));

    // Load column base pointers for each needed column.
    // columnPtrs is void** — an array of pointers.
    // To get column i: columnPtrs[i] (each element is 8 bytes = pointer size).
    const auto& colIndices = scan_->columnIndices;
    std::vector<Ptr<Int8>> colBases;
    colBases.reserve(colIndices.size());

    for (uint32_t colIdx : colIndices) {
        auto idx = cg.constInt64(static_cast<int64_t>(colIdx));
        auto elemPtr = Ptr<Int8>(bld.emitGEP(colPtrsBase.op(), ir::TypeId::PTR, idx.op()));
        auto colBase = Ptr<Int8>(bld.emitLoad(ir::TypeId::PTR, elemPtr.op()));
        colBases.push_back(colBase);
    }

    // Emit scan loop: for i = 0 to rowCount.
    auto zero = cg.constInt64(0);
    LoopBuilder loop(cg, {zero.op()}, {ir::TypeId::INT64});
    auto loopI = loop.getLoopVar<Int64>(0);

    // Load column values for this row.
    std::vector<SQLValue> tuple;
    tuple.reserve(colIndices.size());

    for (size_t c = 0; c < colIndices.size(); ++c) {
        uint32_t colIdx = colIndices[c];

        // Determine the SQL type of this column from catalog.
        SQLType sqlType = SQLType::integer();  // default
        for (const auto& colDef : scan_->table->columns) {
            if (colDef.columnIndex == colIdx) {
                sqlType = colDef.type;
                break;
            }
        }

        auto irType = sqlTypeToIR(sqlType.id);
        // GEP into the column array: colBase + i * elementSize.
        auto elemPtr = Ptr<Int8>(bld.emitGEP(colBases[c].op(), irType, loopI.op()));
        auto value = bld.emitLoad(irType, elemPtr.op());
        auto notNull = cg.constBool(false);
        tuple.emplace_back(value, notNull.op(), sqlType);
    }

    // Apply filter predicate if present.
    if (scan_->filter != nullptr) {
        auto filterResult = evaluateExpr(ctx, scan_->filter, tuple, colIndices);
        IfBuilder ifPass(cg, Bool(filterResult.value()));
        parent_->consume(ctx, tuple);
        ifPass.done();
    } else {
        parent_->consume(ctx, tuple);
    }

    // Loop increment and continue condition.
    auto one = cg.constInt64(1);
    auto nextI = loopI + one;
    auto continueLoop = nextI < rowCount;
    loop.done(continueLoop, {nextI.op()});
}

void TableScanTranslator::consume(CompilationContext& /*ctx*/,
                                  const std::vector<SQLValue>& /*tuple*/) {
    // TableScan is a leaf — it does not consume tuples from a child.
    assert(false && "TableScan has no child to consume from");
}

SQLValue TableScanTranslator::evaluateExpr(CompilationContext& ctx, const binder::BoundExpr* expr,
                                           const std::vector<SQLValue>& tuple,
                                           const std::vector<uint32_t>& columnIndices) {
    auto& cg = ctx.codegen;

    switch (expr->kind) {
        case binder::BoundExprKind::COLUMN_REF: {
            const auto* col = static_cast<const binder::BoundColumnRef*>(expr);
            // Find the position of this column in the pruned column list.
            for (size_t i = 0; i < columnIndices.size(); ++i) {
                if (columnIndices[i] == col->columnIndex) {
                    return tuple[i];
                }
            }
            assert(false && "Column not found in scan output");
            return tuple[0];
        }
        case binder::BoundExprKind::LITERAL: {
            const auto* lit = static_cast<const binder::BoundLiteral*>(expr);
            return SQLValue::makeConst(cg, lit->intValue, lit->resultType);
        }
        case binder::BoundExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const binder::BoundBinaryExpr*>(expr);
            auto lhs = evaluateExpr(ctx, bin->left, tuple, columnIndices);
            auto rhs = evaluateExpr(ctx, bin->right, tuple, columnIndices);

            // Map parser BinaryOp to codegen ops.
            switch (bin->op) {
                case parser::BinaryOp::ADD:
                    return SQLValue::evaluateBinary(cg, BinaryOp::ADD, lhs, rhs);
                case parser::BinaryOp::SUB:
                    return SQLValue::evaluateBinary(cg, BinaryOp::SUB, lhs, rhs);
                case parser::BinaryOp::MUL:
                    return SQLValue::evaluateBinary(cg, BinaryOp::MUL, lhs, rhs);
                case parser::BinaryOp::DIV:
                    return SQLValue::evaluateBinary(cg, BinaryOp::DIV, lhs, rhs);
                case parser::BinaryOp::LE:
                    return SQLValue::evaluateCompare(cg, CmpOp::LE, lhs, rhs);
                case parser::BinaryOp::LT:
                    return SQLValue::evaluateCompare(cg, CmpOp::LT, lhs, rhs);
                case parser::BinaryOp::GE:
                    return SQLValue::evaluateCompare(cg, CmpOp::GE, lhs, rhs);
                case parser::BinaryOp::GT:
                    return SQLValue::evaluateCompare(cg, CmpOp::GT, lhs, rhs);
                case parser::BinaryOp::EQ:
                    return SQLValue::evaluateCompare(cg, CmpOp::EQ, lhs, rhs);
                case parser::BinaryOp::NE:
                    return SQLValue::evaluateCompare(cg, CmpOp::NE, lhs, rhs);
                case parser::BinaryOp::AND:
                    // Logical AND: both must be true.
                    return {cg.land(Bool(lhs.value()), Bool(rhs.value())).op(),
                            cg.lor(Bool(lhs.nullFlag()), Bool(rhs.nullFlag())).op(),
                            SQLType::boolean()};
                case parser::BinaryOp::OR:
                    // Logical OR.
                    return {cg.lor(Bool(lhs.value()), Bool(rhs.value())).op(),
                            cg.lor(Bool(lhs.nullFlag()), Bool(rhs.nullFlag())).op(),
                            SQLType::boolean()};
                default:
                    assert(false && "Unsupported binary op in filter");
                    return lhs;
            }
        }
        case binder::BoundExprKind::CAST: {
            const auto* cast = static_cast<const binder::BoundCast*>(expr);
            auto inner = evaluateExpr(ctx, cast->expr, tuple, columnIndices);
            return inner.castTo(cg, cast->resultType);
        }
        case binder::BoundExprKind::UNARY_EXPR: {
            const auto* un = static_cast<const binder::BoundUnaryExpr*>(expr);
            auto operand = evaluateExpr(ctx, un->operand, tuple, columnIndices);
            if (un->op == parser::UnaryOp::NEG) {
                auto zero = SQLValue::makeConst(cg, 0, operand.type());
                return SQLValue::evaluateBinary(cg, BinaryOp::SUB, zero, operand);
            }
            if (un->op == parser::UnaryOp::NOT) {
                return {cg.lnot(Bool(operand.value())).op(), operand.nullFlag(),
                        SQLType::boolean()};
            }
            return operand;
        }
        default:
            assert(false && "Unsupported expression kind in scan filter");
            return tuple[0];
    }
}

}  // namespace umbra::codegen
