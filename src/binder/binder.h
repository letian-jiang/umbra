#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "catalog/catalog.h"
#include "parser/ast.h"

namespace umbra::binder {

/// Binding error with descriptive message
class BindError : public std::runtime_error {
   public:
    explicit BindError(const std::string& message) : std::runtime_error(message) {}
};

/// Result of binding: the context owns all nodes, root points to top-level operator
struct BindResult {
    BoundLogicalOperator* root{nullptr};
};

/// Binder: resolves names, infers types, folds constants, analyzes aggregates.
class Binder {
   public:
    explicit Binder(const Catalog& catalog);

    /// Bind a parsed SELECT statement into a logical plan.
    BindResult bind(const parser::SelectStmt& stmt);

   private:
    const Catalog& catalog_;

    // ── Scope ────────────────────────────────────────────────────────────────
    struct ScopeEntry {
        const TableDef* table;
        uint32_t tableIndex;
    };

    // Tables in scope (by table name and alias)
    std::unordered_map<std::string, ScopeEntry> scope_;

    // The single table (MVP: single-table queries only)
    const TableDef* currentTable_{nullptr};
    uint32_t currentTableIndex_{0};

    void addTableToScope(const TableDef* table, const std::string& alias);
    const ColumnDef* resolveColumn(const std::string& tableName, const std::string& colName);

    // ── Node Allocation ──────────────────────────────────────────────────────
    std::vector<std::unique_ptr<BoundExpr>> ownedExprs_;
    std::vector<std::unique_ptr<BoundLogicalOperator>> ownedOps_;

    template <typename T, typename... Args>
    T* allocExpr(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = ptr.get();
        ownedExprs_.push_back(std::move(ptr));
        return raw;
    }

    template <typename T, typename... Args>
    T* allocOp(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = ptr.get();
        ownedOps_.push_back(std::move(ptr));
        return raw;
    }

    // ── Binding Methods ──────────────────────────────────────────────────────
    BoundLogicalOperator* bindFrom(const parser::TableRef* tableRef);
    BoundLogicalOperator* bindWhere(BoundLogicalOperator* child, const parser::Expr* whereClause);
    BoundExpr* bindExpr(const parser::Expr* expr);
    BoundExpr* bindColumnRef(const parser::ColumnRef* colRef);
    BoundExpr* bindLiteral(const parser::Literal* lit);
    BoundExpr* bindBinaryExpr(const parser::BinaryExpr* binExpr);
    BoundExpr* bindUnaryExpr(const parser::UnaryExpr* unExpr);
    BoundExpr* bindFuncCall(const parser::FuncCall* funcCall);
    BoundExpr* bindDateArith(const parser::DateArith* dateArith);

    // ── Type Inference ───────────────────────────────────────────────────────
    struct TypeResolution {
        BoundExpr* left;
        BoundExpr* right;
        SQLType resultType;
    };

    TypeResolution resolveTypes(parser::BinaryOp op, BoundExpr* left, BoundExpr* right);
    static SQLType findCommonType(SQLType left, SQLType right);
    static SQLType inferResultType(parser::BinaryOp op, SQLType inputType);
    BoundExpr* insertCast(BoundExpr* expr, SQLType targetType);

    // ── Constant Folding ─────────────────────────────────────────────────────
    static int32_t evaluateConstDate(const parser::Literal* dateLit);
    static int64_t evaluateConstInterval(const parser::Literal* intervalLit);

    // ── Aggregate Analysis ───────────────────────────────────────────────────
    struct AggregateAnalysis {
        std::vector<BoundExpr*> groupByExprs;
        std::vector<BoundAggCall*> aggregates;
        std::vector<BoundExpr*> projectionExprs;
        std::vector<std::string> projectionAliases;
    };

    AggregateAnalysis analyzeAggregates(const parser::SelectStmt& stmt);

    // Check if an expression tree contains aggregates
    static bool containsAggregate(const parser::Expr* expr);

    // Find or add an aggregate, returning its index
    uint32_t findOrAddAggregate(std::vector<BoundAggCall*>& aggs, BoundAggCall* newAgg);

    // Rewrite expressions to use AggRef and GroupRef
    BoundExpr* rewriteExprForProjection(const parser::Expr* expr,
                                        const std::vector<BoundExpr*>& groupByExprs,
                                        std::vector<BoundAggCall*>& aggregates);

    // Check if two bound expressions match (for group-by validation)
    static bool exprMatchesGroupBy(const parser::Expr* expr,
                                   const std::vector<parser::Expr*>& groupByExprs);

    // Validate that non-aggregate expressions are in GROUP BY
    static void validateGroupBy(const parser::Expr* expr,
                                const std::vector<parser::Expr*>& groupByExprs);
};

}  // namespace umbra::binder
