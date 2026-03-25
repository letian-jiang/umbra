#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "parser/ast.h"

namespace umbra::binder {

// ── Enums ────────────────────────────────────────────────────────────────────

enum class BoundExprKind {
    COLUMN_REF,
    LITERAL,
    BINARY_EXPR,
    UNARY_EXPR,
    AGG_CALL,
    CAST,
    AGG_REF,
    GROUP_REF,
};

enum class AggFunc {
    SUM,
    COUNT,
};

// ── Bound Expression Nodes ──────────────────────────────────────────────────

/// Base bound expression with resolved type
struct BoundExpr {
    BoundExprKind kind;
    SQLType resultType;

    BoundExpr(BoundExprKind k, SQLType type) : kind(k), resultType(type) {}
    virtual ~BoundExpr() = default;

    BoundExpr(const BoundExpr&) = default;
    BoundExpr& operator=(const BoundExpr&) = default;
    BoundExpr(BoundExpr&&) = default;
    BoundExpr& operator=(BoundExpr&&) = default;
};

/// Resolved column reference
struct BoundColumnRef : BoundExpr {
    uint32_t tableIndex;   // table index (0 for single-table)
    uint32_t columnIndex;  // column position in table
    std::string columnName;

    BoundColumnRef(uint32_t ti, uint32_t ci, SQLType type, std::string name)
        : BoundExpr(BoundExprKind::COLUMN_REF, type),
          tableIndex(ti),
          columnIndex(ci),
          columnName(std::move(name)) {}
};

/// Literal value (int64, date, string)
struct BoundLiteral : BoundExpr {
    int64_t intValue{0};     // for INTEGER, BIGINT, DECIMAL, DATE
    std::string strValue{};  // for VARCHAR

    explicit BoundLiteral(SQLType type, int64_t val = 0)
        : BoundExpr(BoundExprKind::LITERAL, type), intValue(val) {}
    BoundLiteral(SQLType type, std::string val)
        : BoundExpr(BoundExprKind::LITERAL, type), strValue(std::move(val)) {}
};

/// Binary expression with resolved types
struct BoundBinaryExpr : BoundExpr {
    parser::BinaryOp op;
    BoundExpr* left;
    BoundExpr* right;

    BoundBinaryExpr(parser::BinaryOp o, BoundExpr* l, BoundExpr* r, SQLType type)
        : BoundExpr(BoundExprKind::BINARY_EXPR, type), op(o), left(l), right(r) {}
};

/// Unary expression with resolved type
struct BoundUnaryExpr : BoundExpr {
    parser::UnaryOp op;
    BoundExpr* operand;

    BoundUnaryExpr(parser::UnaryOp o, BoundExpr* operand, SQLType type)
        : BoundExpr(BoundExprKind::UNARY_EXPR, type), op(o), operand(operand) {}
};

/// Aggregate function call (after AVG decomposition: only SUM and COUNT)
struct BoundAggCall : BoundExpr {
    AggFunc func;
    BoundExpr* arg;  // nullptr for COUNT(*)

    BoundAggCall(AggFunc f, BoundExpr* a, SQLType type)
        : BoundExpr(BoundExprKind::AGG_CALL, type), func(f), arg(a) {}
};

/// Implicit or explicit type cast
struct BoundCast : BoundExpr {
    BoundExpr* expr;
    SQLType sourceType;

    BoundCast(BoundExpr* e, SQLType src, SQLType tgt)
        : BoundExpr(BoundExprKind::CAST, tgt), expr(e), sourceType(src) {}
};

/// Reference to an aggregate result (by index into aggregate list)
struct BoundAggRef : BoundExpr {
    uint32_t aggIndex;

    BoundAggRef(uint32_t idx, SQLType type)
        : BoundExpr(BoundExprKind::AGG_REF, type), aggIndex(idx) {}
};

/// Reference to a group-by key (by index into group-by list)
struct BoundGroupRef : BoundExpr {
    uint32_t groupIndex;

    BoundGroupRef(uint32_t idx, SQLType type)
        : BoundExpr(BoundExprKind::GROUP_REF, type), groupIndex(idx) {}
};

}  // namespace umbra::binder
