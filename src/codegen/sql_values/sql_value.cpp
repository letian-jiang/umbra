#include "codegen/sql_values/sql_value.h"

#include <algorithm>
#include <cassert>

namespace umbra::codegen {

// ── Utility ─────────────────────────────────────────────────────────────────

ir::TypeId sqlTypeToIR(SQLTypeId typeId) {
    switch (typeId) {
        case SQLTypeId::BOOLEAN:
            return ir::TypeId::BOOL;
        case SQLTypeId::INTEGER:
        case SQLTypeId::DATE:
            return ir::TypeId::INT32;
        case SQLTypeId::BIGINT:
        case SQLTypeId::DECIMAL:
            return ir::TypeId::INT64;
        case SQLTypeId::VARCHAR:
            return ir::TypeId::PTR;
    }
    return ir::TypeId::INT64;
}

/// Helper: compute scale factor (10^scale).
static int64_t scaleFactor(uint8_t scale) {
    int64_t result = 1;
    for (uint8_t i = 0; i < scale; ++i) {
        result *= 10;
    }
    return result;
}

// ── SQLValue constructors ───────────────────────────────────────────────────

SQLValue::SQLValue(ir::IROp value, ir::IROp nullFlag, SQLType type)
    : value_(value), nullFlag_(nullFlag), type_(type) {}

SQLValue::SQLValue(CodegenContext& ctx, ir::IROp value, SQLType type)
    : value_(value), nullFlag_(ctx.constBool(false).op()), type_(type) {}

SQLValue SQLValue::makeNull(CodegenContext& ctx, SQLType type) {
    auto irType = sqlTypeToIR(type.id);
    auto zeroVal = ctx.builder().emitConst(irType, 0);
    auto nullFlag = ctx.constBool(true).op();
    return {zeroVal, nullFlag, type};
}

SQLValue SQLValue::makeConst(CodegenContext& ctx, int64_t value, SQLType type) {
    auto irType = sqlTypeToIR(type.id);
    auto val = ctx.builder().emitConst(irType, static_cast<uint64_t>(value));
    auto notNull = ctx.constBool(false).op();
    return {val, notNull, type};
}

// ── Type inference ──────────────────────────────────────────────────────────

SQLType SQLValue::inferBinaryResultType(BinaryOp op, const SQLType& lhs, const SQLType& rhs) {
    // DECIMAL * DECIMAL → DECIMAL with combined scale (will be rescaled)
    if (lhs.id == SQLTypeId::DECIMAL && rhs.id == SQLTypeId::DECIMAL) {
        if (op == BinaryOp::MUL) {
            // Scale after mul = lhs.scale + rhs.scale, then we rescale to max(lhs.scale, rhs.scale)
            auto maxScale = std::max(lhs.scale, rhs.scale);
            auto maxPrec = std::max(lhs.precision, rhs.precision);
            return SQLType::decimal(maxPrec, maxScale);
        }
        if (op == BinaryOp::DIV) {
            return SQLType::decimal(lhs.precision, lhs.scale);
        }
        // ADD/SUB: same scale (caller ensures matching scales)
        auto maxScale = std::max(lhs.scale, rhs.scale);
        auto maxPrec = std::max(lhs.precision, rhs.precision);
        return SQLType::decimal(maxPrec, maxScale);
    }

    // DECIMAL op BIGINT or BIGINT op DECIMAL → DECIMAL
    if (lhs.id == SQLTypeId::DECIMAL || rhs.id == SQLTypeId::DECIMAL) {
        const auto& dec = lhs.id == SQLTypeId::DECIMAL ? lhs : rhs;
        return SQLType::decimal(dec.precision, dec.scale);
    }

    // BIGINT operations
    if (lhs.id == SQLTypeId::BIGINT || rhs.id == SQLTypeId::BIGINT) {
        return SQLType::bigint();
    }

    // DATE comparison is handled by compare, not binary
    if (lhs.id == SQLTypeId::DATE || rhs.id == SQLTypeId::DATE) {
        return SQLType::integer();
    }

    // INTEGER operations
    return SQLType::integer();
}

// ── Cast ────────────────────────────────────────────────────────────────────

SQLValue SQLValue::castIfNeeded(CodegenContext& ctx, const SQLValue& val, SQLType targetType) {
    if (val.type_.id == targetType.id && val.type_.scale == targetType.scale) {
        return val;
    }

    return val.castTo(ctx, targetType);
}

SQLValue SQLValue::castTo(CodegenContext& ctx, SQLType targetType) const {
    if (type_.id == targetType.id && type_.scale == targetType.scale) {
        return *this;
    }

    ir::IROp newValue = value_;
    auto srcIR = sqlTypeToIR(type_.id);
    auto dstIR = sqlTypeToIR(targetType.id);

    // INTEGER → BIGINT: sign extend
    if (type_.id == SQLTypeId::INTEGER && targetType.id == SQLTypeId::BIGINT) {
        newValue = ctx.builder().emitSExt(ir::TypeId::INT64, value_);
    }
    // INTEGER → DECIMAL: sign extend then scale
    else if (type_.id == SQLTypeId::INTEGER && targetType.id == SQLTypeId::DECIMAL) {
        newValue = ctx.builder().emitSExt(ir::TypeId::INT64, value_);
        auto sf = ctx.builder().emitConst(ir::TypeId::INT64,
                                          static_cast<uint64_t>(scaleFactor(targetType.scale)));
        newValue = ctx.builder().emitMul(ir::TypeId::INT64, newValue, sf);
    }
    // BIGINT → DECIMAL: multiply by scale factor
    else if (type_.id == SQLTypeId::BIGINT && targetType.id == SQLTypeId::DECIMAL) {
        auto sf = ctx.builder().emitConst(ir::TypeId::INT64,
                                          static_cast<uint64_t>(scaleFactor(targetType.scale)));
        newValue = ctx.builder().emitMul(ir::TypeId::INT64, value_, sf);
    }
    // DECIMAL → DECIMAL with different scale
    else if (type_.id == SQLTypeId::DECIMAL && targetType.id == SQLTypeId::DECIMAL &&
             type_.scale != targetType.scale) {
        if (type_.scale < targetType.scale) {
            // Scale up
            auto diff = static_cast<uint8_t>(targetType.scale - type_.scale);
            auto sf = ctx.builder().emitConst(ir::TypeId::INT64,
                                              static_cast<uint64_t>(scaleFactor(diff)));
            newValue = ctx.builder().emitMul(ir::TypeId::INT64, value_, sf);
        } else {
            // Scale down (truncate — rounding toward zero)
            auto diff = static_cast<uint8_t>(type_.scale - targetType.scale);
            auto sf = ctx.builder().emitConst(ir::TypeId::INT64,
                                              static_cast<uint64_t>(scaleFactor(diff)));
            newValue = ctx.builder().emitDiv(ir::TypeId::INT64, value_, sf);
        }
    }
    // BIGINT → INTEGER: truncate
    else if (type_.id == SQLTypeId::BIGINT && targetType.id == SQLTypeId::INTEGER) {
        newValue = ctx.builder().emitTrunc(ir::TypeId::INT32, value_);
    }
    // If source and dest have different IR sizes but we didn't handle above, use sext/trunc
    else if (srcIR != dstIR) {
        if (ir::typeSize(srcIR) < ir::typeSize(dstIR)) {
            newValue = ctx.builder().emitSExt(dstIR, value_);
        } else {
            newValue = ctx.builder().emitTrunc(dstIR, value_);
        }
    }

    return {newValue, nullFlag_, targetType};
}

// ── Binary arithmetic ───────────────────────────────────────────────────────

ir::IROp SQLValue::emitArithmeticOp(CodegenContext& ctx, BinaryOp op, ir::TypeId irType,
                                    ir::IROp lhs, ir::IROp rhs) {
    auto& bld = ctx.builder();
    switch (op) {
        case BinaryOp::ADD:
            return bld.emitAdd(irType, lhs, rhs);
        case BinaryOp::SUB:
            return bld.emitSub(irType, lhs, rhs);
        case BinaryOp::MUL:
            return bld.emitMul(irType, lhs, rhs);
        case BinaryOp::DIV:
            return bld.emitDiv(irType, lhs, rhs);
    }
    return ir::INVALID_OP;
}

ir::IROp SQLValue::rescaleDecimalMul(CodegenContext& ctx, ir::IROp value, uint8_t extraScale) {
    if (extraScale == 0) {
        return value;
    }
    auto sf =
        ctx.builder().emitConst(ir::TypeId::INT64, static_cast<uint64_t>(scaleFactor(extraScale)));
    return ctx.builder().emitDiv(ir::TypeId::INT64, value, sf);
}

ir::IROp SQLValue::prescaleDecimalDiv(CodegenContext& ctx, ir::IROp value, uint8_t scale) {
    if (scale == 0) {
        return value;
    }
    auto sf = ctx.builder().emitConst(ir::TypeId::INT64, static_cast<uint64_t>(scaleFactor(scale)));
    return ctx.builder().emitMul(ir::TypeId::INT64, value, sf);
}

SQLValue SQLValue::evaluateBinary(CodegenContext& ctx, BinaryOp op, const SQLValue& lhs,
                                  const SQLValue& rhs) {
    // 1. Infer result type.
    SQLType resultType = inferBinaryResultType(op, lhs.type_, rhs.type_);

    // 2. Determine common type for operands.
    SQLType commonType = resultType;
    if (op == BinaryOp::MUL && lhs.type_.id == SQLTypeId::DECIMAL &&
        rhs.type_.id == SQLTypeId::DECIMAL) {
        // For decimal multiply, don't cast — multiply raw values then rescale.
        // common type is the individual operand types.
    }

    // 3. Cast operands if needed.
    SQLValue castLhs = lhs;
    SQLValue castRhs = rhs;

    if (op == BinaryOp::MUL && lhs.type_.id == SQLTypeId::DECIMAL &&
        rhs.type_.id == SQLTypeId::DECIMAL) {
        // No pre-cast needed for decimal * decimal.
    } else if (op == BinaryOp::DIV && lhs.type_.id == SQLTypeId::DECIMAL &&
               rhs.type_.id == SQLTypeId::DECIMAL) {
        // For decimal division, pre-scale the dividend to preserve precision.
        // lhs / rhs where both are scaled: (lhs * 10^rhs.scale) / rhs
        auto prescaled = prescaleDecimalDiv(ctx, lhs.value_, rhs.type_.scale);
        castLhs = SQLValue(prescaled, lhs.nullFlag_, lhs.type_);
    } else if (op == BinaryOp::DIV && lhs.type_.id == SQLTypeId::DECIMAL &&
               rhs.type_.id == SQLTypeId::BIGINT) {
        // DECIMAL / BIGINT: direct division preserves scale.
        // No cast needed for rhs — but we need rhs as Int64.
        castRhs = castIfNeeded(ctx, rhs, SQLType::bigint());
    } else {
        castLhs = castIfNeeded(ctx, lhs, commonType);
        castRhs = castIfNeeded(ctx, rhs, commonType);
    }

    // 4. NULL propagation: result is null if either operand is null.
    auto resultNull = ctx.builder().emitLOr(castLhs.nullFlag_, castRhs.nullFlag_);

    // 5. Emit arithmetic.
    auto irType = sqlTypeToIR(resultType.id);
    auto resultValue = emitArithmeticOp(ctx, op, irType, castLhs.value_, castRhs.value_);

    // 6. DECIMAL rescaling for multiplication.
    if (op == BinaryOp::MUL && lhs.type_.id == SQLTypeId::DECIMAL &&
        rhs.type_.id == SQLTypeId::DECIMAL) {
        // After mul, effective scale = lhs.scale + rhs.scale
        // Result scale = resultType.scale = max(lhs.scale, rhs.scale)
        // Extra scale = lhs.scale + rhs.scale - resultType.scale
        auto extraScale =
            static_cast<uint8_t>(lhs.type_.scale + rhs.type_.scale - resultType.scale);
        resultValue = rescaleDecimalMul(ctx, resultValue, extraScale);
    }

    return {resultValue, resultNull, resultType};
}

// ── Comparison ──────────────────────────────────────────────────────────────

ir::IROp SQLValue::emitCompareOp(CodegenContext& ctx, CmpOp op, ir::TypeId irType, ir::IROp lhs,
                                 ir::IROp rhs) {
    auto& bld = ctx.builder();
    switch (op) {
        case CmpOp::EQ:
            return bld.emitCmpEq(irType, lhs, rhs);
        case CmpOp::NE:
            return bld.emitCmpNe(irType, lhs, rhs);
        case CmpOp::LT:
            return bld.emitCmpLt(irType, lhs, rhs);
        case CmpOp::LE:
            return bld.emitCmpLe(irType, lhs, rhs);
        case CmpOp::GT:
            return bld.emitCmpGt(irType, lhs, rhs);
        case CmpOp::GE:
            return bld.emitCmpGe(irType, lhs, rhs);
    }
    return ir::INVALID_OP;
}

SQLValue SQLValue::evaluateCompare(CodegenContext& ctx, CmpOp op, const SQLValue& lhs,
                                   const SQLValue& rhs) {
    // Determine common type for comparison.
    SQLType commonType = inferBinaryResultType(BinaryOp::ADD, lhs.type_, rhs.type_);

    // For DATE comparisons, use the date type directly.
    if (lhs.type_.id == SQLTypeId::DATE && rhs.type_.id == SQLTypeId::DATE) {
        commonType = SQLType::date();
    }

    // For DECIMAL comparisons, ensure matching scales.
    if (lhs.type_.id == SQLTypeId::DECIMAL && rhs.type_.id == SQLTypeId::DECIMAL) {
        auto maxScale = std::max(lhs.type_.scale, rhs.type_.scale);
        auto maxPrec = std::max(lhs.type_.precision, rhs.type_.precision);
        commonType = SQLType::decimal(maxPrec, maxScale);
    }

    // Cast if needed.
    SQLValue castLhs = castIfNeeded(ctx, lhs, commonType);
    SQLValue castRhs = castIfNeeded(ctx, rhs, commonType);

    // NULL propagation.
    auto resultNull = ctx.builder().emitLOr(castLhs.nullFlag_, castRhs.nullFlag_);

    // Emit comparison.
    auto irType = sqlTypeToIR(commonType.id);
    auto cmpResult = emitCompareOp(ctx, op, irType, castLhs.value_, castRhs.value_);

    return {cmpResult, resultNull, SQLType::boolean()};
}

}  // namespace umbra::codegen
