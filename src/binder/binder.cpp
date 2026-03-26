#include "binder/binder.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "storage/tbl_loader.h"

namespace umbra::binder {

// ── Constructor ──────────────────────────────────────────────────────────────

Binder::Binder(const Catalog& catalog) : catalog_(catalog) {}

// ── Top-level bind ───────────────────────────────────────────────────────────

BindResult Binder::bind(const parser::SelectStmt& stmt) {
    // Clear state from any previous bind
    scope_.clear();
    currentTable_ = nullptr;
    currentTableIndex_ = 0;
    ownedExprs_.clear();
    ownedOps_.clear();

    // 1. FROM
    if (stmt.fromTable == nullptr) {
        throw BindError("missing FROM clause");
    }
    auto* scan = bindFrom(stmt.fromTable);

    // 2. WHERE
    auto* afterFilter = bindWhere(scan, stmt.whereClause);

    // 3. SELECT + GROUP BY (aggregate analysis)
    if (!stmt.groupBy.empty()) {
        auto analysis = analyzeAggregates(stmt);

        // Build aggregate node
        auto* agg = allocOp<BoundAggregate>(afterFilter);
        agg->groupBy = std::move(analysis.groupByExprs);
        agg->aggregates = std::move(analysis.aggregates);

        // Build projection node
        auto* proj = allocOp<BoundProjection>(agg);
        proj->expressions = std::move(analysis.projectionExprs);
        proj->aliases = std::move(analysis.projectionAliases);

        // 4. ORDER BY
        if (!stmt.orderBy.empty()) {
            auto* sort = allocOp<BoundSort>(proj);
            for (const auto& item : stmt.orderBy) {
                BoundOrderByItem boundItem;
                boundItem.expr = bindExpr(item.expr);
                boundItem.ascending = item.ascending;
                sort->orderBy.push_back(boundItem);
            }
            return {sort};
        }

        return {proj};
    }

    // No GROUP BY: simple projection
    auto* proj = allocOp<BoundProjection>(afterFilter);
    for (const auto* selectExpr : stmt.selectList) {
        const auto* aliased = static_cast<const parser::AliasedExpr*>(selectExpr);
        proj->expressions.push_back(bindExpr(aliased->expr));
        proj->aliases.push_back(aliased->alias);
    }

    // ORDER BY
    if (!stmt.orderBy.empty()) {
        auto* sort = allocOp<BoundSort>(proj);
        for (const auto& item : stmt.orderBy) {
            BoundOrderByItem boundItem;
            boundItem.expr = bindExpr(item.expr);
            boundItem.ascending = item.ascending;
            sort->orderBy.push_back(boundItem);
        }
        return {sort};
    }

    return {proj};
}

// ── FROM binding ─────────────────────────────────────────────────────────────

BoundLogicalOperator* Binder::bindFrom(const parser::TableRef* tableRef) {
    const TableDef* table = catalog_.lookupTable(tableRef->tableName);
    if (table == nullptr) {
        throw BindError("table not found: " + tableRef->tableName);
    }

    addTableToScope(table, tableRef->alias);

    auto* scan = allocOp<BoundScan>(table);
    for (const auto& col : table->columns) {
        auto* colRef =
            allocExpr<BoundColumnRef>(currentTableIndex_, col.columnIndex, col.type, col.name);
        scan->outputColumns.push_back(colRef);
    }
    return scan;
}

// ── WHERE binding ────────────────────────────────────────────────────────────

BoundLogicalOperator* Binder::bindWhere(BoundLogicalOperator* child,
                                        const parser::Expr* whereClause) {
    if (whereClause == nullptr) {
        return child;
    }

    BoundExpr* predicate = bindExpr(whereClause);

    // Ensure predicate is BOOLEAN
    if (predicate->resultType.id != SQLTypeId::BOOLEAN) {
        throw BindError("WHERE clause must be a boolean expression");
    }

    return allocOp<BoundFilter>(child, predicate);
}

// ── Expression binding ───────────────────────────────────────────────────────

BoundExpr* Binder::bindExpr(const parser::Expr* expr) {
    if (expr == nullptr) {
        throw BindError("null expression");
    }

    switch (expr->kind) {
        case parser::ExprKind::COLUMN_REF:
            return bindColumnRef(static_cast<const parser::ColumnRef*>(expr));
        case parser::ExprKind::LITERAL:
            return bindLiteral(static_cast<const parser::Literal*>(expr));
        case parser::ExprKind::BINARY_EXPR:
            return bindBinaryExpr(static_cast<const parser::BinaryExpr*>(expr));
        case parser::ExprKind::UNARY_EXPR:
            return bindUnaryExpr(static_cast<const parser::UnaryExpr*>(expr));
        case parser::ExprKind::FUNC_CALL:
            return bindFuncCall(static_cast<const parser::FuncCall*>(expr));
        case parser::ExprKind::DATE_ARITH:
            return bindDateArith(static_cast<const parser::DateArith*>(expr));
        case parser::ExprKind::ALIASED_EXPR: {
            const auto* aliased = static_cast<const parser::AliasedExpr*>(expr);
            return bindExpr(aliased->expr);
        }
        case parser::ExprKind::CAST_EXPR:
            throw BindError("explicit CAST not supported in MVP");
    }
    throw BindError("unknown expression kind");
}

BoundExpr* Binder::bindColumnRef(const parser::ColumnRef* colRef) {
    const ColumnDef* col = resolveColumn(colRef->tableName, colRef->columnName);
    return allocExpr<BoundColumnRef>(currentTableIndex_, col->columnIndex, col->type, col->name);
}

BoundExpr* Binder::bindLiteral(const parser::Literal* lit) {
    switch (lit->literalType) {
        case parser::LiteralType::INTEGER:
            return allocExpr<BoundLiteral>(SQLType::integer(), lit->intValue);
        case parser::LiteralType::STRING:
            return allocExpr<BoundLiteral>(SQLType::varchar(0), lit->strValue);
        case parser::LiteralType::DATE: {
            int32_t daysSinceEpoch = evaluateConstDate(lit);
            return allocExpr<BoundLiteral>(SQLType::date(), static_cast<int64_t>(daysSinceEpoch));
        }
        case parser::LiteralType::DECIMAL: {
            int64_t val = parseDecimal(lit->strValue, 2);
            return allocExpr<BoundLiteral>(SQLType::decimal(15, 2), val);
        }
        case parser::LiteralType::INTERVAL:
            throw BindError("standalone INTERVAL not supported; use DATE arithmetic");
    }
    throw BindError("unknown literal type");
}

BoundExpr* Binder::bindBinaryExpr(const parser::BinaryExpr* binExpr) {
    auto* left = bindExpr(binExpr->left);
    auto* right = bindExpr(binExpr->right);

    // Convert string literal to CHAR when compared with a CHAR column.
    auto convertStringToChar = [this](BoundExpr*& expr, const SQLType& targetType) {
        if (targetType.id != SQLTypeId::CHAR || expr->kind != BoundExprKind::LITERAL ||
            expr->resultType.id != SQLTypeId::VARCHAR) {
            return;
        }
        auto* lit = static_cast<BoundLiteral*>(expr);
        if (lit->strValue.size() > targetType.maxLen) {
            throw BindError("string literal too long for CHAR(" +
                            std::to_string(targetType.maxLen) + ")");
        }
        if (targetType.maxLen == 1) {
            if (lit->strValue.empty()) {
                throw BindError("empty string literal cannot be compared with CHAR(1)");
            }
            expr = allocExpr<BoundLiteral>(SQLType::fixedChar(1),
                                           static_cast<int64_t>(lit->strValue[0]));
        } else {
            throw BindError("CHAR(" + std::to_string(targetType.maxLen) +
                            ") comparison not yet supported");
        }
    };
    convertStringToChar(right, left->resultType);
    convertStringToChar(left, right->resultType);

    auto resolved = resolveTypes(binExpr->op, left, right);
    return allocExpr<BoundBinaryExpr>(binExpr->op, resolved.left, resolved.right,
                                      resolved.resultType);
}

BoundExpr* Binder::bindUnaryExpr(const parser::UnaryExpr* unExpr) {
    auto* operand = bindExpr(unExpr->operand);
    return allocExpr<BoundUnaryExpr>(unExpr->op, operand, operand->resultType);
}

BoundExpr* Binder::bindFuncCall(const parser::FuncCall* funcCall) {
    const auto& name = funcCall->funcName;

    if (name == "sum") {
        if (funcCall->args.size() != 1) {
            throw BindError("SUM requires exactly one argument");
        }
        auto* arg = bindExpr(funcCall->args[0]);
        // SUM result type matches input type
        return allocExpr<BoundAggCall>(AggFunc::SUM, arg, arg->resultType);
    }

    if (name == "avg") {
        if (funcCall->args.size() != 1) {
            throw BindError("AVG requires exactly one argument");
        }
        auto* arg = bindExpr(funcCall->args[0]);
        // AVG result is DECIMAL
        SQLType resultType = arg->resultType;
        if (resultType.id == SQLTypeId::INTEGER) {
            resultType = SQLType::decimal(15, 2);
        }
        // For AVG, we create a placeholder BoundAggCall with SUM func
        // AVG decomposition happens in analyzeAggregates
        // But here we mark it as SUM temporarily; the caller handles AVG
        return allocExpr<BoundAggCall>(AggFunc::SUM, arg, resultType);
    }

    if (name == "count") {
        if (funcCall->isStar) {
            return allocExpr<BoundAggCall>(AggFunc::COUNT, nullptr, SQLType::bigint());
        }
        if (funcCall->args.size() != 1) {
            throw BindError("COUNT requires zero or one argument");
        }
        auto* arg = bindExpr(funcCall->args[0]);
        return allocExpr<BoundAggCall>(AggFunc::COUNT, arg, SQLType::bigint());
    }

    throw BindError("unknown function: " + name);
}

BoundExpr* Binder::bindDateArith(const parser::DateArith* dateArith) {
    // Constant fold: evaluate at bind time
    if (dateArith->dateExpr->kind != parser::ExprKind::LITERAL ||
        dateArith->interval->kind != parser::ExprKind::LITERAL) {
        throw BindError("DATE arithmetic requires constant operands");
    }

    const auto* dateLit = static_cast<const parser::Literal*>(dateArith->dateExpr);
    const auto* intervalLit = static_cast<const parser::Literal*>(dateArith->interval);

    int32_t dateDays = evaluateConstDate(dateLit);
    int64_t intervalDays = evaluateConstInterval(intervalLit);

    int32_t result = 0;
    if (dateArith->op == parser::BinaryOp::SUB) {
        result = dateDays - static_cast<int32_t>(intervalDays);
    } else if (dateArith->op == parser::BinaryOp::ADD) {
        result = dateDays + static_cast<int32_t>(intervalDays);
    } else {
        throw BindError("unsupported DATE arithmetic operator");
    }

    return allocExpr<BoundLiteral>(SQLType::date(), static_cast<int64_t>(result));
}

// ── Scope Management ─────────────────────────────────────────────────────────

void Binder::addTableToScope(const TableDef* table, const std::string& alias) {
    currentTable_ = table;
    ScopeEntry entry{table, currentTableIndex_};

    // Register by table name
    scope_[table->name] = entry;

    // Also register by alias if present
    if (!alias.empty()) {
        scope_[alias] = entry;
    }
}

const ColumnDef* Binder::resolveColumn(const std::string& tableName, const std::string& colName) {
    const TableDef* table = nullptr;

    if (!tableName.empty()) {
        auto it = scope_.find(tableName);
        if (it == scope_.end()) {
            throw BindError("table not found in scope: " + tableName);
        }
        table = it->second.table;
    } else {
        // No qualifier: search in the current table (single-table MVP)
        if (currentTable_ == nullptr) {
            throw BindError("no table in scope");
        }
        table = currentTable_;
    }

    const ColumnDef* col = catalog_.lookupColumn(*table, colName);
    if (col == nullptr) {
        throw BindError("column not found: " + colName);
    }
    return col;
}

// ── Type Inference ───────────────────────────────────────────────────────────

Binder::TypeResolution Binder::resolveTypes(parser::BinaryOp op, BoundExpr* left,
                                            BoundExpr* right) {
    // Comparison operators always return BOOLEAN
    bool isComparison =
        (op == parser::BinaryOp::EQ || op == parser::BinaryOp::NE || op == parser::BinaryOp::LT ||
         op == parser::BinaryOp::LE || op == parser::BinaryOp::GT || op == parser::BinaryOp::GE);

    bool isLogical = (op == parser::BinaryOp::AND || op == parser::BinaryOp::OR);

    if (isLogical) {
        // Both sides must be BOOLEAN
        return {left, right, SQLType::boolean()};
    }

    SQLType leftType = left->resultType;
    SQLType rightType = right->resultType;

    if (leftType.id == rightType.id) {
        // Same type family
        if (leftType.id == SQLTypeId::DECIMAL) {
            SQLType resultType = inferResultType(op, leftType);
            // If precisions/scales differ, use the wider one for the inputs
            if (leftType.precision != rightType.precision || leftType.scale != rightType.scale) {
                SQLType common = findCommonType(leftType, rightType);
                if (leftType.precision != common.precision || leftType.scale != common.scale) {
                    left = insertCast(left, common);
                }
                if (rightType.precision != common.precision || rightType.scale != common.scale) {
                    right = insertCast(right, common);
                }
                if (!isComparison) {
                    resultType = inferResultType(op, common);
                }
            }
            if (isComparison) {
                return {left, right, SQLType::boolean()};
            }
            return {left, right, resultType};
        }
        if (isComparison) {
            return {left, right, SQLType::boolean()};
        }
        return {left, right, inferResultType(op, leftType)};
    }

    // Different types: find common type and insert casts
    SQLType common = findCommonType(leftType, rightType);
    if (common.id == SQLTypeId::BOOLEAN) {
        throw BindError("incompatible types in binary expression");
    }

    if (leftType.id != common.id || leftType.precision != common.precision ||
        leftType.scale != common.scale) {
        left = insertCast(left, common);
    }
    if (rightType.id != common.id || rightType.precision != common.precision ||
        rightType.scale != common.scale) {
        right = insertCast(right, common);
    }

    if (isComparison) {
        return {left, right, SQLType::boolean()};
    }
    return {left, right, inferResultType(op, common)};
}

SQLType Binder::findCommonType(SQLType left, SQLType right) {
    // Same type
    if (left.id == right.id) {
        if (left.id == SQLTypeId::DECIMAL) {
            return SQLType::decimal(std::max(left.precision, right.precision),
                                    std::max(left.scale, right.scale));
        }
        return left;
    }

    // CHAR(1) and VARCHAR are compatible string types.
    // CHAR(n>1) comparison is not yet implemented.
    if ((left.id == SQLTypeId::CHAR && left.maxLen == 1 && right.id == SQLTypeId::VARCHAR) ||
        (left.id == SQLTypeId::VARCHAR && right.id == SQLTypeId::CHAR && right.maxLen == 1)) {
        return left.id == SQLTypeId::CHAR ? left : right;
    }

    // Promotion: INTEGER -> BIGINT -> DECIMAL
    auto typeRank = [](SQLTypeId id) -> int {
        switch (id) {
            case SQLTypeId::INTEGER:
                return 0;
            case SQLTypeId::BIGINT:
                return 1;
            case SQLTypeId::DECIMAL:
                return 2;
            default:
                return -1;
        }
    };

    int leftRank = typeRank(left.id);
    int rightRank = typeRank(right.id);

    if (leftRank < 0 || rightRank < 0) {
        // Can't find common type (e.g., DATE vs VARCHAR)
        return SQLType::boolean();  // sentinel for error
    }

    if (leftRank > rightRank) {
        // Left is the wider type
        if (left.id == SQLTypeId::DECIMAL) {
            return left;
        }
        return left;
    }
    // Right is the wider type
    if (right.id == SQLTypeId::DECIMAL) {
        return right;
    }
    return right;
}

SQLType Binder::inferResultType(parser::BinaryOp op, SQLType inputType) {
    switch (op) {
        case parser::BinaryOp::ADD:
        case parser::BinaryOp::SUB:
            return inputType;
        case parser::BinaryOp::MUL:
            if (inputType.id == SQLTypeId::DECIMAL) {
                // For multiplication, scale adds but we cap at reasonable values
                return SQLType::decimal(std::min(static_cast<int>(inputType.precision) * 2, 38),
                                        std::min(static_cast<int>(inputType.scale) * 2, 18));
            }
            return inputType;
        case parser::BinaryOp::DIV:
        case parser::BinaryOp::MOD:
            return inputType;
        case parser::BinaryOp::EQ:
        case parser::BinaryOp::NE:
        case parser::BinaryOp::LT:
        case parser::BinaryOp::LE:
        case parser::BinaryOp::GT:
        case parser::BinaryOp::GE:
        case parser::BinaryOp::AND:
        case parser::BinaryOp::OR:
            return SQLType::boolean();
    }
    return inputType;
}

BoundExpr* Binder::insertCast(BoundExpr* expr, SQLType targetType) {
    return allocExpr<BoundCast>(expr, expr->resultType, targetType);
}

// ── Constant Folding ─────────────────────────────────────────────────────────

int32_t Binder::evaluateConstDate(const parser::Literal* dateLit) {
    if (dateLit->literalType != parser::LiteralType::DATE) {
        throw BindError("expected DATE literal");
    }
    return parseDate(dateLit->strValue);
}

int64_t Binder::evaluateConstInterval(const parser::Literal* intervalLit) {
    if (intervalLit->literalType != parser::LiteralType::INTERVAL) {
        throw BindError("expected INTERVAL literal");
    }
    return intervalLit->intervalValue;
}

// ── Aggregate Analysis ───────────────────────────────────────────────────────

bool Binder::containsAggregate(const parser::Expr* expr) {
    if (expr == nullptr) {
        return false;
    }

    switch (expr->kind) {
        case parser::ExprKind::FUNC_CALL: {
            const auto* fc = static_cast<const parser::FuncCall*>(expr);
            if (fc->funcName == "sum" || fc->funcName == "avg" || fc->funcName == "count") {
                return true;
            }
            // Check arguments
            for (const auto* arg : fc->args) {
                if (containsAggregate(arg)) {
                    return true;
                }
            }
            return false;
        }
        case parser::ExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const parser::BinaryExpr*>(expr);
            return containsAggregate(bin->left) || containsAggregate(bin->right);
        }
        case parser::ExprKind::UNARY_EXPR: {
            const auto* un = static_cast<const parser::UnaryExpr*>(expr);
            return containsAggregate(un->operand);
        }
        case parser::ExprKind::ALIASED_EXPR: {
            const auto* ae = static_cast<const parser::AliasedExpr*>(expr);
            return containsAggregate(ae->expr);
        }
        default:
            return false;
    }
}

uint32_t Binder::findOrAddAggregate(std::vector<BoundAggCall*>& aggs, BoundAggCall* newAgg) {
    // Check for duplicate aggregates
    for (uint32_t i = 0; i < aggs.size(); ++i) {
        auto* existing = aggs[i];
        if (existing->func != newAgg->func) {
            continue;
        }

        // COUNT(*) matches COUNT(*)
        if (newAgg->func == AggFunc::COUNT && newAgg->arg == nullptr && existing->arg == nullptr) {
            return i;
        }

        // SUM(col_x) matches SUM(col_x)
        if (newAgg->arg != nullptr && existing->arg != nullptr) {
            if (newAgg->arg->kind == BoundExprKind::COLUMN_REF &&
                existing->arg->kind == BoundExprKind::COLUMN_REF) {
                auto* newCol = static_cast<BoundColumnRef*>(newAgg->arg);
                auto* existCol = static_cast<BoundColumnRef*>(existing->arg);
                if (newCol->tableIndex == existCol->tableIndex &&
                    newCol->columnIndex == existCol->columnIndex) {
                    return i;
                }
            }
        }
    }

    // Not found: add new
    auto idx = static_cast<uint32_t>(aggs.size());
    aggs.push_back(newAgg);
    return idx;
}

bool Binder::exprMatchesGroupBy(const parser::Expr* expr,
                                const std::vector<parser::Expr*>& groupByExprs) {
    if (expr == nullptr) {
        return false;
    }

    // For column refs, check if any group-by expr is the same column
    if (expr->kind == parser::ExprKind::COLUMN_REF) {
        const auto* colRef = static_cast<const parser::ColumnRef*>(expr);
        for (const auto* gbExpr : groupByExprs) {
            if (gbExpr->kind == parser::ExprKind::COLUMN_REF) {
                const auto* gbCol = static_cast<const parser::ColumnRef*>(gbExpr);
                if (colRef->columnName == gbCol->columnName) {
                    return true;
                }
            }
        }
    }

    return false;
}

void Binder::validateGroupBy(const parser::Expr* expr,
                             const std::vector<parser::Expr*>& groupByExprs) {
    if (expr == nullptr) {
        return;
    }

    // If this is an aggregate, it's fine — no need to check group-by
    if (containsAggregate(expr)) {
        return;
    }

    // If it's an aliased expression, check the inner expression
    if (expr->kind == parser::ExprKind::ALIASED_EXPR) {
        const auto* ae = static_cast<const parser::AliasedExpr*>(expr);
        validateGroupBy(ae->expr, groupByExprs);
        return;
    }

    // Column refs must be in GROUP BY
    if (expr->kind == parser::ExprKind::COLUMN_REF) {
        if (!exprMatchesGroupBy(expr, groupByExprs)) {
            const auto* colRef = static_cast<const parser::ColumnRef*>(expr);
            throw BindError("column " + colRef->columnName +
                            " must appear in GROUP BY clause or be in an aggregate function");
        }
        return;
    }

    // Recurse into sub-expressions
    if (expr->kind == parser::ExprKind::BINARY_EXPR) {
        const auto* bin = static_cast<const parser::BinaryExpr*>(expr);
        validateGroupBy(bin->left, groupByExprs);
        validateGroupBy(bin->right, groupByExprs);
        return;
    }
    if (expr->kind == parser::ExprKind::UNARY_EXPR) {
        const auto* un = static_cast<const parser::UnaryExpr*>(expr);
        validateGroupBy(un->operand, groupByExprs);
    }
}

BoundExpr* Binder::rewriteExprForProjection(const parser::Expr* expr,
                                            const std::vector<BoundExpr*>& groupByExprs,
                                            std::vector<BoundAggCall*>& aggregates) {
    if (expr == nullptr) {
        throw BindError("null expression in projection rewrite");
    }

    // Check if this is a column ref matching a group-by expr
    if (expr->kind == parser::ExprKind::COLUMN_REF) {
        const auto* colRef = static_cast<const parser::ColumnRef*>(expr);
        for (uint32_t i = 0; i < groupByExprs.size(); ++i) {
            if (groupByExprs[i]->kind == BoundExprKind::COLUMN_REF) {
                auto* gbCol = static_cast<BoundColumnRef*>(groupByExprs[i]);
                if (colRef->columnName == gbCol->columnName) {
                    return allocExpr<BoundGroupRef>(i, gbCol->resultType);
                }
            }
        }
        throw BindError("column " + colRef->columnName + " not in GROUP BY");
    }

    // Aggregate function call
    if (expr->kind == parser::ExprKind::FUNC_CALL) {
        const auto* fc = static_cast<const parser::FuncCall*>(expr);
        if (fc->funcName == "sum") {
            auto* boundAgg = static_cast<BoundAggCall*>(bindFuncCall(fc));
            uint32_t idx = findOrAddAggregate(aggregates, boundAgg);
            return allocExpr<BoundAggRef>(idx, aggregates[idx]->resultType);
        }
        if (fc->funcName == "count") {
            auto* boundAgg = static_cast<BoundAggCall*>(bindFuncCall(fc));
            uint32_t idx = findOrAddAggregate(aggregates, boundAgg);
            return allocExpr<BoundAggRef>(idx, aggregates[idx]->resultType);
        }
        if (fc->funcName == "avg") {
            // AVG(x) -> SUM(x) / COUNT(*)
            if (fc->args.size() != 1) {
                throw BindError("AVG requires exactly one argument");
            }

            // Create SUM(x)
            auto* sumArg = bindExpr(fc->args[0]);
            SQLType sumType = sumArg->resultType;
            if (sumType.id == SQLTypeId::INTEGER) {
                sumType = SQLType::bigint();
            }
            auto* sumAgg = allocExpr<BoundAggCall>(AggFunc::SUM, sumArg, sumType);
            uint32_t sumIdx = findOrAddAggregate(aggregates, sumAgg);

            // Create COUNT(*)
            auto* countAgg = allocExpr<BoundAggCall>(AggFunc::COUNT, nullptr, SQLType::bigint());
            uint32_t countIdx = findOrAddAggregate(aggregates, countAgg);

            // AVG = SUM / COUNT
            auto* sumRef = allocExpr<BoundAggRef>(sumIdx, aggregates[sumIdx]->resultType);
            auto* countRef = allocExpr<BoundAggRef>(countIdx, aggregates[countIdx]->resultType);

            // Determine result type for division
            SQLType divResultType = aggregates[sumIdx]->resultType;
            if (divResultType.id == SQLTypeId::BIGINT) {
                divResultType = SQLType::decimal(15, 2);
            }

            // Cast count to match sum type for division
            BoundExpr* castCount = countRef;
            if (countRef->resultType.id != sumRef->resultType.id) {
                castCount = insertCast(countRef, sumRef->resultType);
            }

            return allocExpr<BoundBinaryExpr>(parser::BinaryOp::DIV, sumRef, castCount,
                                              divResultType);
        }
    }

    // Binary expression
    if (expr->kind == parser::ExprKind::BINARY_EXPR) {
        const auto* bin = static_cast<const parser::BinaryExpr*>(expr);
        auto* left = rewriteExprForProjection(bin->left, groupByExprs, aggregates);
        auto* right = rewriteExprForProjection(bin->right, groupByExprs, aggregates);
        auto resolved = resolveTypes(bin->op, left, right);
        return allocExpr<BoundBinaryExpr>(bin->op, resolved.left, resolved.right,
                                          resolved.resultType);
    }

    // Unary expression
    if (expr->kind == parser::ExprKind::UNARY_EXPR) {
        const auto* un = static_cast<const parser::UnaryExpr*>(expr);
        auto* operand = rewriteExprForProjection(un->operand, groupByExprs, aggregates);
        return allocExpr<BoundUnaryExpr>(un->op, operand, operand->resultType);
    }

    // Literal
    if (expr->kind == parser::ExprKind::LITERAL) {
        return bindLiteral(static_cast<const parser::Literal*>(expr));
    }

    // DateArith (constant fold)
    if (expr->kind == parser::ExprKind::DATE_ARITH) {
        return bindDateArith(static_cast<const parser::DateArith*>(expr));
    }

    throw BindError("unsupported expression in projection rewrite");
}

Binder::AggregateAnalysis Binder::analyzeAggregates(const parser::SelectStmt& stmt) {
    AggregateAnalysis result;

    // 1. Bind group-by expressions
    for (const auto* gbExpr : stmt.groupBy) {
        result.groupByExprs.push_back(bindExpr(gbExpr));
    }

    // 2. Validate: non-aggregate SELECT expressions must be in GROUP BY
    for (const auto* selectExpr : stmt.selectList) {
        validateGroupBy(selectExpr, stmt.groupBy);
    }

    // 3. Validate: GROUP BY must not contain aggregates
    for (const auto* gbExpr : stmt.groupBy) {
        if (containsAggregate(gbExpr)) {
            throw BindError("GROUP BY must not contain aggregate functions");
        }
    }

    // 4. Rewrite SELECT list: replace aggregates with AggRef, group-by with GroupRef
    for (const auto* selectExpr : stmt.selectList) {
        const auto* aliased = static_cast<const parser::AliasedExpr*>(selectExpr);
        auto* rewritten =
            rewriteExprForProjection(aliased->expr, result.groupByExprs, result.aggregates);
        result.projectionExprs.push_back(rewritten);
        result.projectionAliases.push_back(aliased->alias);
    }

    return result;
}

}  // namespace umbra::binder
