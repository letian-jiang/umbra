#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace umbra::parser {

// Forward declarations
struct Expr;

// ── Enums ────────────────────────────────────────────────────────────────────

enum class BinaryOp {
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    AND,
    OR,
    EQ,
    NE,
    LT,
    LE,
    GT,
    GE,
};

enum class UnaryOp {
    NEG,
    NOT,
};

enum class LiteralType {
    INTEGER,
    STRING,
    DATE,
    INTERVAL,
    DECIMAL,
};

/// Interval field (only DAY supported for MVP)
enum class IntervalField {
    DAY,
};

// ── Expression Nodes ─────────────────────────────────────────────────────────

/// Expression node tag for RTTI
enum class ExprKind {
    COLUMN_REF,
    LITERAL,
    BINARY_EXPR,
    UNARY_EXPR,
    FUNC_CALL,
    CAST_EXPR,
    DATE_ARITH,
    ALIASED_EXPR,
};

/// Base expression node (tagged union via kind field)
struct Expr {
    ExprKind kind;
    int32_t location{-1};  // source position from PG parser

    explicit Expr(ExprKind k) : kind(k) {}
    virtual ~Expr() = default;

    Expr(const Expr&) = default;
    Expr& operator=(const Expr&) = default;
    Expr(Expr&&) = default;
    Expr& operator=(Expr&&) = default;
};

/// Column reference: e.g. "l_shipdate" or "t.l_shipdate"
struct ColumnRef : Expr {
    std::string tableName;   // optional qualifier
    std::string columnName;  // column name

    ColumnRef() : Expr(ExprKind::COLUMN_REF) {}
};

/// Literal value
struct Literal : Expr {
    LiteralType literalType{};
    int64_t intValue{0};       // for INTEGER
    std::string strValue;      // for STRING, DATE, DECIMAL
    int64_t intervalValue{0};  // for INTERVAL (number of days, etc.)
    IntervalField intervalField{IntervalField::DAY};

    Literal() : Expr(ExprKind::LITERAL) {}
};

/// Binary expression: left op right
struct BinaryExpr : Expr {
    BinaryOp op{};
    Expr* left{nullptr};
    Expr* right{nullptr};

    BinaryExpr() : Expr(ExprKind::BINARY_EXPR) {}
};

/// Unary expression: op operand
struct UnaryExpr : Expr {
    UnaryOp op{};
    Expr* operand{nullptr};

    UnaryExpr() : Expr(ExprKind::UNARY_EXPR) {}
};

/// Function call: SUM(x), AVG(x), COUNT(*)
struct FuncCall : Expr {
    std::string funcName;     // lowercase: "sum", "avg", "count"
    std::vector<Expr*> args;  // arguments (empty for COUNT(*))
    bool isStar{false};       // true for COUNT(*)

    FuncCall() : Expr(ExprKind::FUNC_CALL) {}
};

/// Cast expression (explicit type cast)
struct CastExpr : Expr {
    Expr* expr{nullptr};
    std::string targetTypeName;  // type name string for display

    CastExpr() : Expr(ExprKind::CAST_EXPR) {}
};

/// Date arithmetic: date_expr +/- interval
struct DateArith : Expr {
    BinaryOp op{};  // ADD or SUB
    Expr* dateExpr{nullptr};
    Expr* interval{nullptr};

    DateArith() : Expr(ExprKind::DATE_ARITH) {}
};

/// Aliased expression in SELECT list
struct AliasedExpr : Expr {
    Expr* expr{nullptr};
    std::string alias;  // empty if no alias

    AliasedExpr() : Expr(ExprKind::ALIASED_EXPR) {}
};

// ── Order By ─────────────────────────────────────────────────────────────────

struct OrderByItem {
    Expr* expr{nullptr};
    bool ascending{true};
};

// ── Table Reference ──────────────────────────────────────────────────────────

struct TableRef {
    std::string tableName;
    std::string alias;  // optional
};

// ── Statement ────────────────────────────────────────────────────────────────

struct SelectStmt {
    std::vector<Expr*> selectList;     // SELECT clause (AliasedExpr nodes)
    TableRef* fromTable{nullptr};      // FROM clause
    Expr* whereClause{nullptr};        // WHERE clause (optional)
    std::vector<Expr*> groupBy;        // GROUP BY clause
    std::vector<OrderByItem> orderBy;  // ORDER BY clause
};

}  // namespace umbra::parser
