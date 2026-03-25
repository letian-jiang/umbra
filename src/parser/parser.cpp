#include "parser/parser.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// libpg_query C headers
extern "C" {
#include <pg_query.h>
#include <protobuf/pg_query.pb-c.h>
}

namespace umbra::parser {

namespace {

/// Helper: extract string from a PG String node.
std::string getStringValue(const PgQuery__Node* node) {
    if (node == nullptr) {
        return "";
    }
    if (node->node_case == PG_QUERY__NODE__NODE_STRING) {
        return node->string->sval != nullptr ? node->string->sval : "";
    }
    return "";
}

/// Helper: extract operator name from A_Expr name list.
/// The name list is a list of String nodes; for simple operators it has one element.
std::string getOperatorName(const PgQuery__AExpr* expr) {
    if (expr->n_name > 0 && expr->name[0] != nullptr) {
        return getStringValue(expr->name[0]);
    }
    return "";
}

/// Map operator string to BinaryOp.
BinaryOp mapBinaryOp(const std::string& opStr) {
    if (opStr == "+") {
        return BinaryOp::ADD;
    }
    if (opStr == "-") {
        return BinaryOp::SUB;
    }
    if (opStr == "*") {
        return BinaryOp::MUL;
    }
    if (opStr == "/") {
        return BinaryOp::DIV;
    }
    if (opStr == "%") {
        return BinaryOp::MOD;
    }
    if (opStr == "=") {
        return BinaryOp::EQ;
    }
    if (opStr == "<>" || opStr == "!=") {
        return BinaryOp::NE;
    }
    if (opStr == "<") {
        return BinaryOp::LT;
    }
    if (opStr == "<=") {
        return BinaryOp::LE;
    }
    if (opStr == ">") {
        return BinaryOp::GT;
    }
    if (opStr == ">=") {
        return BinaryOp::GE;
    }
    throw ParseError("unsupported operator: " + opStr, -1);
}

/// Extract type name string from a TypeName node.
/// Returns the last name component (e.g. "date", "interval", "int4").
std::string extractTypeName(const PgQuery__TypeName* typeName) {
    if (typeName == nullptr || typeName->n_names == 0) {
        return "";
    }
    // Use the last name component (skip "pg_catalog" prefix if present)
    return getStringValue(typeName->names[typeName->n_names - 1]);
}

/// Check if a TypeName represents INTERVAL and extract the field modifier.
bool isIntervalType(const PgQuery__TypeName* typeName) {
    std::string name = extractTypeName(typeName);
    return name == "interval";
}

/// Check if a TypeName represents DATE.
bool isDateType(const PgQuery__TypeName* typeName) {
    std::string name = extractTypeName(typeName);
    return name == "date";
}

/// RAII wrapper for PgQueryProtobufParseResult.
class PgParseResultGuard {
   public:
    explicit PgParseResultGuard(PgQueryProtobufParseResult r) : result_(r) {}
    ~PgParseResultGuard() { pg_query_free_protobuf_parse_result(result_); }

    PgParseResultGuard(const PgParseResultGuard&) = delete;
    PgParseResultGuard& operator=(const PgParseResultGuard&) = delete;

    PgQueryProtobufParseResult* operator->() { return &result_; }

   private:
    PgQueryProtobufParseResult result_;
};

/// Converter: PG protobuf nodes -> internal AST.
class PgToAstConverter {
   public:
    explicit PgToAstConverter(ASTContext& ctx) : ctx_(ctx) {}

    SelectStmt* convertSelectStmt(const PgQuery__SelectStmt* pgStmt) {
        auto* stmt = ctx_.allocSelectStmt();

        // SELECT list (target_list)
        for (size_t i = 0; i < pgStmt->n_target_list; ++i) {
            auto* node = pgStmt->target_list[i];
            if (node->node_case == PG_QUERY__NODE__NODE_RES_TARGET) {
                stmt->selectList.push_back(convertResTarget(node->res_target));
            }
        }

        // FROM clause
        if (pgStmt->n_from_clause > 0) {
            auto* fromNode = pgStmt->from_clause[0];
            if (fromNode->node_case == PG_QUERY__NODE__NODE_RANGE_VAR) {
                stmt->fromTable = convertRangeVar(fromNode->range_var);
            }
        }

        // WHERE clause
        if (pgStmt->where_clause != nullptr) {
            stmt->whereClause = convertExpr(pgStmt->where_clause);
        }

        // GROUP BY
        for (size_t i = 0; i < pgStmt->n_group_clause; ++i) {
            stmt->groupBy.push_back(convertExpr(pgStmt->group_clause[i]));
        }

        // ORDER BY
        for (size_t i = 0; i < pgStmt->n_sort_clause; ++i) {
            auto* node = pgStmt->sort_clause[i];
            if (node->node_case == PG_QUERY__NODE__NODE_SORT_BY) {
                stmt->orderBy.push_back(convertSortBy(node->sort_by));
            }
        }

        return stmt;
    }

   private:
    ASTContext& ctx_;

    /// Convert a ResTarget to AliasedExpr.
    Expr* convertResTarget(const PgQuery__ResTarget* rt) {
        auto* aliased = ctx_.alloc<AliasedExpr>();
        aliased->location = rt->location;
        if (rt->name != nullptr && std::strlen(rt->name) > 0) {
            aliased->alias = rt->name;
        }
        if (rt->val != nullptr) {
            aliased->expr = convertExpr(rt->val);
        }
        return aliased;
    }

    /// Convert a RangeVar to TableRef.
    TableRef* convertRangeVar(const PgQuery__RangeVar* rv) {
        auto* ref = ctx_.allocTableRef();
        if (rv->relname != nullptr) {
            ref->tableName = rv->relname;
        }
        if (rv->alias != nullptr && rv->alias->aliasname != nullptr &&
            std::strlen(rv->alias->aliasname) > 0) {
            ref->alias = rv->alias->aliasname;
        }
        return ref;
    }

    /// Convert a SortBy to OrderByItem.
    OrderByItem convertSortBy(const PgQuery__SortBy* sb) {
        OrderByItem item;
        if (sb->node != nullptr) {
            item.expr = convertExpr(sb->node);
        }
        // Default and ASC are ascending; DESC is descending
        item.ascending = (sb->sortby_dir != PG_QUERY__SORT_BY_DIR__SORTBY_DESC);
        return item;
    }

    /// Convert any PG Node to an Expr.
    Expr* convertExpr(const PgQuery__Node* node) {
        switch (node->node_case) {
            case PG_QUERY__NODE__NODE_COLUMN_REF:
                return convertColumnRef(node->column_ref);
            case PG_QUERY__NODE__NODE_A_CONST:
                return convertAConst(node->a_const);
            case PG_QUERY__NODE__NODE_A_EXPR:
                return convertAExpr(node->a_expr);
            case PG_QUERY__NODE__NODE_FUNC_CALL:
                return convertFuncCall(node->func_call);
            case PG_QUERY__NODE__NODE_TYPE_CAST:
                return convertTypeCast(node->type_cast);
            case PG_QUERY__NODE__NODE_BOOL_EXPR:
                return convertBoolExpr(node->bool_expr);
            default:
                throw ParseError("unsupported SQL expression type", -1);
        }
    }

    /// Convert ColumnRef.
    Expr* convertColumnRef(const PgQuery__ColumnRef* cr) {
        auto* ref = ctx_.alloc<ColumnRef>();
        ref->location = cr->location;

        if (cr->n_fields == 1) {
            // Simple column name
            ref->columnName = getStringValue(cr->fields[0]);
        } else if (cr->n_fields == 2) {
            // table.column
            ref->tableName = getStringValue(cr->fields[0]);
            ref->columnName = getStringValue(cr->fields[1]);
        }
        return ref;
    }

    /// Convert A_Const (constant value).
    Expr* convertAConst(const PgQuery__AConst* ac) {
        auto* lit = ctx_.alloc<Literal>();
        lit->location = ac->location;

        switch (ac->val_case) {
            case PG_QUERY__A__CONST__VAL_IVAL:
                lit->literalType = LiteralType::INTEGER;
                lit->intValue = ac->ival->ival;
                break;
            case PG_QUERY__A__CONST__VAL_FVAL:
                lit->literalType = LiteralType::DECIMAL;
                lit->strValue = (ac->fval->fval != nullptr) ? ac->fval->fval : "";
                break;
            case PG_QUERY__A__CONST__VAL_SVAL:
                lit->literalType = LiteralType::STRING;
                lit->strValue = (ac->sval->sval != nullptr) ? ac->sval->sval : "";
                break;
            default:
                throw ParseError("unsupported constant type", ac->location);
        }
        return lit;
    }

    /// Convert A_Expr (arithmetic/comparison expression).
    Expr* convertAExpr(const PgQuery__AExpr* ae) {
        if (ae->kind != PG_QUERY__A__EXPR__KIND__AEXPR_OP) {
            throw ParseError("unsupported expression kind", ae->location);
        }

        std::string opStr = getOperatorName(ae);

        // Check for date arithmetic pattern: left is DATE, right is INTERVAL
        if ((opStr == "-" || opStr == "+") && ae->lexpr != nullptr && ae->rexpr != nullptr) {
            if (isDateArithPattern(ae->lexpr, ae->rexpr)) {
                return buildDateArith(ae, opStr);
            }
        }

        // Unary expression: no left operand
        if (ae->lexpr == nullptr && ae->rexpr != nullptr) {
            auto* unary = ctx_.alloc<UnaryExpr>();
            unary->location = ae->location;
            if (opStr == "-") {
                unary->op = UnaryOp::NEG;
            } else {
                throw ParseError("unsupported unary operator: " + opStr, ae->location);
            }
            unary->operand = convertExpr(ae->rexpr);
            return unary;
        }

        // Binary expression
        auto* binExpr = ctx_.alloc<BinaryExpr>();
        binExpr->location = ae->location;
        binExpr->op = mapBinaryOp(opStr);
        if (ae->lexpr != nullptr) {
            binExpr->left = convertExpr(ae->lexpr);
        }
        if (ae->rexpr != nullptr) {
            binExpr->right = convertExpr(ae->rexpr);
        }
        return binExpr;
    }

    /// Check if an expression pair looks like DATE op INTERVAL.
    static bool isDateArithPattern(const PgQuery__Node* left, const PgQuery__Node* right) {
        // left should be TypeCast to date, right should be TypeCast to interval
        bool leftIsDate = false;
        bool rightIsInterval = false;

        if (left->node_case == PG_QUERY__NODE__NODE_TYPE_CAST) {
            leftIsDate = isDateType(left->type_cast->type_name);
        }
        if (right->node_case == PG_QUERY__NODE__NODE_TYPE_CAST) {
            rightIsInterval = isIntervalType(right->type_cast->type_name);
        }

        return leftIsDate && rightIsInterval;
    }

    /// Build a DateArith node from date - interval pattern.
    Expr* buildDateArith(const PgQuery__AExpr* ae, const std::string& opStr) {
        auto* da = ctx_.alloc<DateArith>();
        da->location = ae->location;
        da->op = (opStr == "+") ? BinaryOp::ADD : BinaryOp::SUB;

        // Convert the date expression (TypeCast to date)
        da->dateExpr = convertTypeCastToDateLiteral(ae->lexpr->type_cast);

        // Convert the interval expression (TypeCast to interval)
        da->interval = convertTypeCastToIntervalLiteral(ae->rexpr->type_cast);

        return da;
    }

    /// Convert a TypeCast(A_Const, "date") -> Literal(DATE)
    Expr* convertTypeCastToDateLiteral(const PgQuery__TypeCast* tc) {
        auto* lit = ctx_.alloc<Literal>();
        lit->location = tc->location;
        lit->literalType = LiteralType::DATE;

        if (tc->arg != nullptr && tc->arg->node_case == PG_QUERY__NODE__NODE_A_CONST) {
            const auto* ac = tc->arg->a_const;
            if (ac->val_case == PG_QUERY__A__CONST__VAL_SVAL && ac->sval->sval != nullptr) {
                lit->strValue = ac->sval->sval;
            }
        }
        return lit;
    }

    /// Convert a TypeCast(A_Const, "interval") -> Literal(INTERVAL)
    Expr* convertTypeCastToIntervalLiteral(const PgQuery__TypeCast* tc) {
        auto* lit = ctx_.alloc<Literal>();
        lit->location = tc->location;
        lit->literalType = LiteralType::INTERVAL;
        lit->intervalField = IntervalField::DAY;  // MVP: only DAY

        if (tc->arg != nullptr && tc->arg->node_case == PG_QUERY__NODE__NODE_A_CONST) {
            const auto* ac = tc->arg->a_const;
            if (ac->val_case == PG_QUERY__A__CONST__VAL_SVAL && ac->sval->sval != nullptr) {
                lit->strValue = ac->sval->sval;
                lit->intervalValue = std::stoll(ac->sval->sval);
            } else if (ac->val_case == PG_QUERY__A__CONST__VAL_IVAL) {
                lit->intervalValue = ac->ival->ival;
                lit->strValue = std::to_string(ac->ival->ival);
            }
        }
        return lit;
    }

    /// Convert FuncCall (SUM, AVG, COUNT, etc.)
    Expr* convertFuncCall(const PgQuery__FuncCall* fc) {
        auto* func = ctx_.alloc<FuncCall>();
        func->location = fc->location;

        // Extract function name (last component of qualified name)
        if (fc->n_funcname > 0) {
            func->funcName = getStringValue(fc->funcname[fc->n_funcname - 1]);
            // Normalize to lowercase
            std::transform(func->funcName.begin(), func->funcName.end(), func->funcName.begin(),
                           [](unsigned char c) { return std::tolower(c); });
        }

        // Check for COUNT(*)
        func->isStar = (fc->agg_star != 0);

        // Convert arguments
        for (size_t i = 0; i < fc->n_args; ++i) {
            func->args.push_back(convertExpr(fc->args[i]));
        }

        return func;
    }

    /// Convert TypeCast node.
    /// Special cases: DATE and INTERVAL casts are recognized as Literal nodes.
    Expr* convertTypeCast(const PgQuery__TypeCast* tc) {
        if (tc->type_name != nullptr) {
            if (isDateType(tc->type_name)) {
                return convertTypeCastToDateLiteral(tc);
            }
            if (isIntervalType(tc->type_name)) {
                return convertTypeCastToIntervalLiteral(tc);
            }
        }

        // Generic cast
        auto* cast = ctx_.alloc<CastExpr>();
        cast->location = tc->location;
        cast->targetTypeName = extractTypeName(tc->type_name);
        if (tc->arg != nullptr) {
            cast->expr = convertExpr(tc->arg);
        }
        return cast;
    }

    /// Convert BoolExpr (AND / OR / NOT).
    Expr* convertBoolExpr(const PgQuery__BoolExpr* be) {
        if (be->boolop == PG_QUERY__BOOL_EXPR_TYPE__NOT_EXPR) {
            // NOT is unary
            auto* unary = ctx_.alloc<UnaryExpr>();
            unary->location = be->location;
            unary->op = UnaryOp::NOT;
            if (be->n_args > 0) {
                unary->operand = convertExpr(be->args[0]);
            }
            return unary;
        }

        // AND / OR — chain into binary tree (left-associative)
        BinaryOp op =
            (be->boolop == PG_QUERY__BOOL_EXPR_TYPE__AND_EXPR) ? BinaryOp::AND : BinaryOp::OR;

        if (be->n_args == 0) {
            throw ParseError("empty bool expression", be->location);
        }

        Expr* result = convertExpr(be->args[0]);
        for (size_t i = 1; i < be->n_args; ++i) {
            auto* bin = ctx_.alloc<BinaryExpr>();
            bin->location = be->location;
            bin->op = op;
            bin->left = result;
            bin->right = convertExpr(be->args[i]);
            result = bin;
        }
        return result;
    }
};

}  // namespace

ParseResult Parser::parse(const std::string& sql) {
    PgParseResultGuard guard(pg_query_parse_protobuf(sql.c_str()));

    // Check for syntax errors
    if (guard->error != nullptr) {
        std::string msg =
            (guard->error->message != nullptr) ? guard->error->message : "unknown parse error";
        int pos = guard->error->cursorpos;
        throw ParseError(msg, pos);
    }

    // Deserialize protobuf
    auto* parseResult = pg_query__parse_result__unpack(
        nullptr, guard->parse_tree.len, reinterpret_cast<const uint8_t*>(guard->parse_tree.data));

    if (parseResult == nullptr) {
        throw ParseError("failed to deserialize parse tree", -1);
    }

    // Ensure we have at least one statement
    if (parseResult->n_stmts == 0) {
        pg_query__parse_result__free_unpacked(parseResult, nullptr);
        throw ParseError("empty query", -1);
    }

    // Take only the first statement
    const auto* rawStmt = parseResult->stmts[0];
    if (rawStmt->stmt == nullptr || rawStmt->stmt->node_case != PG_QUERY__NODE__NODE_SELECT_STMT) {
        pg_query__parse_result__free_unpacked(parseResult, nullptr);
        throw ParseError("only SELECT statements are supported", -1);
    }

    ParseResult result;
    PgToAstConverter converter(result.context);
    result.stmt = converter.convertSelectStmt(rawStmt->stmt->select_stmt);

    pg_query__parse_result__free_unpacked(parseResult, nullptr);
    return result;
}

}  // namespace umbra::parser
