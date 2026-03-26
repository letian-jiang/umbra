#pragma once

#include <cstdint>

#include "codegen/api/codegen_context.h"
#include "common/types.h"
#include "ir/opcode.h"

namespace umbra::codegen {

/// Map a SQL type to the corresponding IR TypeId.
ir::TypeId sqlTypeToIR(const SQLType& type);

/// Binary operation kind for SQL value arithmetic.
enum class BinaryOp { ADD, SUB, MUL, DIV };

/// Comparison operation kind for SQL value comparisons.
enum class CmpOp { EQ, NE, LT, LE, GT, GE };

/// SQLValue — Layer 4 of Tidy Tuples.
/// A triple of (value IR ref, null_flag IR ref, SQL type) that handles
/// NULL propagation and type-correct arithmetic automatically.
///
/// Operations on SQLValue do NOT execute; they EMIT IR instructions.
class SQLValue {
   public:
    /// Construct an SQLValue with an explicit null flag.
    SQLValue(ir::IROp value, ir::IROp nullFlag, SQLType type);

    /// Construct a non-null SQLValue (null_flag = const false).
    SQLValue(CodegenContext& ctx, ir::IROp value, SQLType type);

    /// Construct a null SQLValue of the given type.
    static SQLValue makeNull(CodegenContext& ctx, SQLType type);

    /// Construct a non-null constant SQLValue.
    static SQLValue makeConst(CodegenContext& ctx, int64_t value, SQLType type);

    /// Get the value IROp.
    [[nodiscard]] ir::IROp value() const { return value_; }

    /// Get the null flag IROp.
    [[nodiscard]] ir::IROp nullFlag() const { return nullFlag_; }

    /// Get the SQL type.
    [[nodiscard]] const SQLType& type() const { return type_; }

    /// Get the IR type corresponding to this SQL type.
    [[nodiscard]] ir::TypeId irType() const { return sqlTypeToIR(type_); }

    // ── Binary arithmetic ───────────────────────────────────────────────

    /// Evaluate a binary arithmetic operation with NULL propagation.
    static SQLValue evaluateBinary(CodegenContext& ctx, BinaryOp op, const SQLValue& lhs,
                                   const SQLValue& rhs);

    // ── Comparison ──────────────────────────────────────────────────────

    /// Evaluate a comparison, returns BOOLEAN SQLValue with NULL propagation.
    static SQLValue evaluateCompare(CodegenContext& ctx, CmpOp op, const SQLValue& lhs,
                                    const SQLValue& rhs);

    // ── Cast ────────────────────────────────────────────────────────────

    /// Cast this SQLValue to a target SQL type.
    /// Emits the appropriate IR cast instructions.
    [[nodiscard]] SQLValue castTo(CodegenContext& ctx, SQLType targetType) const;

    // ── Convenience operators ───────────────────────────────────────────

    /// Check if this value is null (returns an IROp Bool).
    [[nodiscard]] ir::IROp isNull() const { return nullFlag_; }

   private:
    ir::IROp value_;
    ir::IROp nullFlag_;
    SQLType type_;

    /// Infer the result type for a binary operation.
    static SQLType inferBinaryResultType(BinaryOp op, const SQLType& lhs, const SQLType& rhs);

    /// Cast if needed to a common type.
    static SQLValue castIfNeeded(CodegenContext& ctx, const SQLValue& val, SQLType targetType);

    /// Emit the raw arithmetic IR instruction for the given types.
    static ir::IROp emitArithmeticOp(CodegenContext& ctx, BinaryOp op, ir::TypeId irType,
                                     ir::IROp lhs, ir::IROp rhs);

    /// Emit the raw comparison IR instruction.
    static ir::IROp emitCompareOp(CodegenContext& ctx, CmpOp op, ir::TypeId irType, ir::IROp lhs,
                                  ir::IROp rhs);

    /// Apply DECIMAL rescaling after multiplication.
    static ir::IROp rescaleDecimalMul(CodegenContext& ctx, ir::IROp value, uint8_t extraScale);

    /// Apply DECIMAL scale adjustment for division.
    static ir::IROp prescaleDecimalDiv(CodegenContext& ctx, ir::IROp value, uint8_t scale);
};

}  // namespace umbra::codegen
