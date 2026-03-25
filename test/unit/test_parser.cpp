#include <catch2/catch_test_macros.hpp>
#include <string>

#include "parser/ast.h"
#include "parser/ast_context.h"
#include "parser/ast_printer.h"
#include "parser/parser.h"

using namespace umbra::parser;

// ── TPC-H Q1 ────────────────────────────────────────────────────────────────

static const char* Q1_SQL = R"(
SELECT l_returnflag, l_linestatus,
       SUM(l_quantity) AS sum_qty,
       SUM(l_extendedprice) AS sum_base_price,
       SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       AVG(l_quantity) AS avg_qty,
       AVG(l_extendedprice) AS avg_price,
       AVG(l_discount) AS avg_disc,
       COUNT(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
)";

TEST_CASE("Parse TPC-H Q1", "[parser]") {
    auto result = Parser::parse(Q1_SQL);
    REQUIRE(result.stmt != nullptr);
    const auto& stmt = *result.stmt;

    SECTION("SELECT list has 10 items") {
        REQUIRE(stmt.selectList.size() == 10);
    }

    SECTION("First two items are unaliased column refs") {
        // l_returnflag
        auto* ae0 = static_cast<AliasedExpr*>(stmt.selectList[0]);
        REQUIRE(ae0->kind == ExprKind::ALIASED_EXPR);
        REQUIRE(ae0->alias.empty());
        REQUIRE(ae0->expr->kind == ExprKind::COLUMN_REF);
        auto* cr0 = static_cast<ColumnRef*>(ae0->expr);
        REQUIRE(cr0->columnName == "l_returnflag");

        // l_linestatus
        auto* ae1 = static_cast<AliasedExpr*>(stmt.selectList[1]);
        REQUIRE(ae1->alias.empty());
        REQUIRE(ae1->expr->kind == ExprKind::COLUMN_REF);
        auto* cr1 = static_cast<ColumnRef*>(ae1->expr);
        REQUIRE(cr1->columnName == "l_linestatus");
    }

    SECTION("SUM(l_quantity) AS sum_qty") {
        auto* ae = static_cast<AliasedExpr*>(stmt.selectList[2]);
        REQUIRE(ae->alias == "sum_qty");
        REQUIRE(ae->expr->kind == ExprKind::FUNC_CALL);
        auto* fc = static_cast<FuncCall*>(ae->expr);
        REQUIRE(fc->funcName == "sum");
        REQUIRE(fc->args.size() == 1);
        REQUIRE(fc->args[0]->kind == ExprKind::COLUMN_REF);
        auto* cr = static_cast<ColumnRef*>(fc->args[0]);
        REQUIRE(cr->columnName == "l_quantity");
    }

    SECTION("SUM(l_extendedprice) AS sum_base_price") {
        auto* ae = static_cast<AliasedExpr*>(stmt.selectList[3]);
        REQUIRE(ae->alias == "sum_base_price");
        REQUIRE(ae->expr->kind == ExprKind::FUNC_CALL);
        auto* fc = static_cast<FuncCall*>(ae->expr);
        REQUIRE(fc->funcName == "sum");
        REQUIRE(fc->args.size() == 1);
        auto* cr = static_cast<ColumnRef*>(fc->args[0]);
        REQUIRE(cr->columnName == "l_extendedprice");
    }

    SECTION("SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price") {
        auto* ae = static_cast<AliasedExpr*>(stmt.selectList[4]);
        REQUIRE(ae->alias == "sum_disc_price");
        REQUIRE(ae->expr->kind == ExprKind::FUNC_CALL);
        auto* fc = static_cast<FuncCall*>(ae->expr);
        REQUIRE(fc->funcName == "sum");
        REQUIRE(fc->args.size() == 1);
        // The argument is: l_extendedprice * (1 - l_discount)
        REQUIRE(fc->args[0]->kind == ExprKind::BINARY_EXPR);
        auto* mul = static_cast<BinaryExpr*>(fc->args[0]);
        REQUIRE(mul->op == BinaryOp::MUL);
        REQUIRE(mul->left->kind == ExprKind::COLUMN_REF);
        REQUIRE(mul->right->kind == ExprKind::BINARY_EXPR);
        auto* sub = static_cast<BinaryExpr*>(mul->right);
        REQUIRE(sub->op == BinaryOp::SUB);
    }

    SECTION("SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge") {
        auto* ae = static_cast<AliasedExpr*>(stmt.selectList[5]);
        REQUIRE(ae->alias == "sum_charge");
        REQUIRE(ae->expr->kind == ExprKind::FUNC_CALL);
        auto* fc = static_cast<FuncCall*>(ae->expr);
        REQUIRE(fc->funcName == "sum");
        REQUIRE(fc->args.size() == 1);
        REQUIRE(fc->args[0]->kind == ExprKind::BINARY_EXPR);
    }

    SECTION("AVG functions") {
        // AVG(l_quantity) AS avg_qty
        auto* ae6 = static_cast<AliasedExpr*>(stmt.selectList[6]);
        REQUIRE(ae6->alias == "avg_qty");
        REQUIRE(ae6->expr->kind == ExprKind::FUNC_CALL);
        auto* fc6 = static_cast<FuncCall*>(ae6->expr);
        REQUIRE(fc6->funcName == "avg");

        // AVG(l_extendedprice) AS avg_price
        auto* ae7 = static_cast<AliasedExpr*>(stmt.selectList[7]);
        REQUIRE(ae7->alias == "avg_price");
        auto* fc7 = static_cast<FuncCall*>(ae7->expr);
        REQUIRE(fc7->funcName == "avg");

        // AVG(l_discount) AS avg_disc
        auto* ae8 = static_cast<AliasedExpr*>(stmt.selectList[8]);
        REQUIRE(ae8->alias == "avg_disc");
        auto* fc8 = static_cast<FuncCall*>(ae8->expr);
        REQUIRE(fc8->funcName == "avg");
    }

    SECTION("COUNT(*) AS count_order") {
        auto* ae = static_cast<AliasedExpr*>(stmt.selectList[9]);
        REQUIRE(ae->alias == "count_order");
        REQUIRE(ae->expr->kind == ExprKind::FUNC_CALL);
        auto* fc = static_cast<FuncCall*>(ae->expr);
        REQUIRE(fc->funcName == "count");
        REQUIRE(fc->isStar);
        REQUIRE(fc->args.empty());
    }

    SECTION("FROM table") {
        REQUIRE(stmt.fromTable != nullptr);
        REQUIRE(stmt.fromTable->tableName == "lineitem");
    }

    SECTION("WHERE clause: l_shipdate <= DATE - INTERVAL") {
        REQUIRE(stmt.whereClause != nullptr);
        REQUIRE(stmt.whereClause->kind == ExprKind::BINARY_EXPR);
        auto* le = static_cast<BinaryExpr*>(stmt.whereClause);
        REQUIRE(le->op == BinaryOp::LE);

        // Left side: l_shipdate
        REQUIRE(le->left->kind == ExprKind::COLUMN_REF);
        auto* cr = static_cast<ColumnRef*>(le->left);
        REQUIRE(cr->columnName == "l_shipdate");

        // Right side: DateArith
        REQUIRE(le->right->kind == ExprKind::DATE_ARITH);
        auto* da = static_cast<DateArith*>(le->right);
        REQUIRE(da->op == BinaryOp::SUB);

        // DATE '1998-12-01'
        REQUIRE(da->dateExpr->kind == ExprKind::LITERAL);
        auto* dateLit = static_cast<Literal*>(da->dateExpr);
        REQUIRE(dateLit->literalType == LiteralType::DATE);
        REQUIRE(dateLit->strValue == "1998-12-01");

        // INTERVAL '90' DAY
        REQUIRE(da->interval->kind == ExprKind::LITERAL);
        auto* intervalLit = static_cast<Literal*>(da->interval);
        REQUIRE(intervalLit->literalType == LiteralType::INTERVAL);
        REQUIRE(intervalLit->intervalValue == 90);
    }

    SECTION("GROUP BY has 2 items") {
        REQUIRE(stmt.groupBy.size() == 2);
        REQUIRE(stmt.groupBy[0]->kind == ExprKind::COLUMN_REF);
        auto* gb0 = static_cast<ColumnRef*>(stmt.groupBy[0]);
        REQUIRE(gb0->columnName == "l_returnflag");

        auto* gb1 = static_cast<ColumnRef*>(stmt.groupBy[1]);
        REQUIRE(gb1->columnName == "l_linestatus");
    }

    SECTION("ORDER BY has 2 items, both ASC") {
        REQUIRE(stmt.orderBy.size() == 2);
        REQUIRE(stmt.orderBy[0].ascending);
        REQUIRE(stmt.orderBy[0].expr->kind == ExprKind::COLUMN_REF);
        auto* ob0 = static_cast<ColumnRef*>(stmt.orderBy[0].expr);
        REQUIRE(ob0->columnName == "l_returnflag");

        REQUIRE(stmt.orderBy[1].ascending);
        auto* ob1 = static_cast<ColumnRef*>(stmt.orderBy[1].expr);
        REQUIRE(ob1->columnName == "l_linestatus");
    }
}

// ── AST Printer ──────────────────────────────────────────────────────────────

TEST_CASE("AST printer produces output", "[parser]") {
    auto result = Parser::parse(Q1_SQL);
    REQUIRE(result.stmt != nullptr);
    std::string printed = ASTPrinter::print(*result.stmt);
    REQUIRE(!printed.empty());
    REQUIRE(printed.find("SelectStmt") != std::string::npos);
    REQUIRE(printed.find("select_list") != std::string::npos);
    REQUIRE(printed.find("from_table") != std::string::npos);
    REQUIRE(printed.find("where_clause") != std::string::npos);
    REQUIRE(printed.find("group_by") != std::string::npos);
    REQUIRE(printed.find("order_by") != std::string::npos);
    REQUIRE(printed.find("lineitem") != std::string::npos);
    REQUIRE(printed.find("DateArith") != std::string::npos);
    REQUIRE(printed.find("FuncCall") != std::string::npos);
}

TEST_CASE("AST printExpr", "[parser]") {
    auto result = Parser::parse("SELECT 1 + 2");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(!result.stmt->selectList.empty());
    std::string printed = ASTPrinter::printExpr(result.stmt->selectList[0]);
    REQUIRE(!printed.empty());
    REQUIRE(printed.find("AliasedExpr") != std::string::npos);
}

// ── Edge cases ───────────────────────────────────────────────────────────────

TEST_CASE("Syntax error produces ParseError", "[parser]") {
    REQUIRE_THROWS_AS(Parser::parse("SELEC x FROM t"), ParseError);
}

TEST_CASE("Empty query produces ParseError", "[parser]") {
    // Empty or whitespace-only query: libpg_query may return 0 stmts
    // or a syntax error depending on version.
    REQUIRE_THROWS_AS(Parser::parse(""), ParseError);
}

TEST_CASE("Non-SELECT statement produces ParseError", "[parser]") {
    REQUIRE_THROWS_AS(Parser::parse("INSERT INTO t VALUES (1)"), ParseError);
}

TEST_CASE("DATE literal parsing", "[parser]") {
    auto result = Parser::parse("SELECT DATE '2024-01-15'");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->selectList.size() == 1);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::LITERAL);
    auto* lit = static_cast<Literal*>(ae->expr);
    REQUIRE(lit->literalType == LiteralType::DATE);
    REQUIRE(lit->strValue == "2024-01-15");
}

TEST_CASE("INTERVAL literal parsing", "[parser]") {
    auto result = Parser::parse("SELECT INTERVAL '30' DAY");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->selectList.size() == 1);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::LITERAL);
    auto* lit = static_cast<Literal*>(ae->expr);
    REQUIRE(lit->literalType == LiteralType::INTERVAL);
    REQUIRE(lit->intervalValue == 30);
}

TEST_CASE("DATE arithmetic expression", "[parser]") {
    auto result = Parser::parse("SELECT DATE '1998-12-01' - INTERVAL '60' DAY");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::DATE_ARITH);
    auto* da = static_cast<DateArith*>(ae->expr);
    REQUIRE(da->op == BinaryOp::SUB);
    REQUIRE(da->dateExpr->kind == ExprKind::LITERAL);
    REQUIRE(da->interval->kind == ExprKind::LITERAL);
    auto* intervalLit = static_cast<Literal*>(da->interval);
    REQUIRE(intervalLit->intervalValue == 60);
}

TEST_CASE("DATE addition expression", "[parser]") {
    auto result = Parser::parse("SELECT DATE '2024-01-01' + INTERVAL '120' DAY");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::DATE_ARITH);
    auto* da = static_cast<DateArith*>(ae->expr);
    REQUIRE(da->op == BinaryOp::ADD);
}

TEST_CASE("Simple SELECT with WHERE", "[parser]") {
    auto result = Parser::parse("SELECT a, b FROM t WHERE a > 5");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->selectList.size() == 2);
    REQUIRE(result.stmt->fromTable != nullptr);
    REQUIRE(result.stmt->fromTable->tableName == "t");
    REQUIRE(result.stmt->whereClause != nullptr);
    auto* cmp = static_cast<BinaryExpr*>(result.stmt->whereClause);
    REQUIRE(cmp->op == BinaryOp::GT);
}

TEST_CASE("ORDER BY DESC", "[parser]") {
    auto result = Parser::parse("SELECT a FROM t ORDER BY a DESC");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->orderBy.size() == 1);
    REQUIRE(!result.stmt->orderBy[0].ascending);
}

TEST_CASE("COUNT(*) function", "[parser]") {
    auto result = Parser::parse("SELECT COUNT(*) FROM t");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    auto* fc = static_cast<FuncCall*>(ae->expr);
    REQUIRE(fc->funcName == "count");
    REQUIRE(fc->isStar);
    REQUIRE(fc->args.empty());
}

TEST_CASE("Qualified column name", "[parser]") {
    auto result = Parser::parse("SELECT t.a FROM t");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::COLUMN_REF);
    auto* cr = static_cast<ColumnRef*>(ae->expr);
    REQUIRE(cr->tableName == "t");
    REQUIRE(cr->columnName == "a");
}

TEST_CASE("Multiple comparison operators", "[parser]") {
    SECTION("less than") {
        auto result = Parser::parse("SELECT a FROM t WHERE a < 10");
        auto* cmp = static_cast<BinaryExpr*>(result.stmt->whereClause);
        REQUIRE(cmp->op == BinaryOp::LT);
    }

    SECTION("greater than or equal") {
        auto result = Parser::parse("SELECT a FROM t WHERE a >= 10");
        auto* cmp = static_cast<BinaryExpr*>(result.stmt->whereClause);
        REQUIRE(cmp->op == BinaryOp::GE);
    }

    SECTION("equality") {
        auto result = Parser::parse("SELECT a FROM t WHERE a = 10");
        auto* cmp = static_cast<BinaryExpr*>(result.stmt->whereClause);
        REQUIRE(cmp->op == BinaryOp::EQ);
    }

    SECTION("not equal") {
        auto result = Parser::parse("SELECT a FROM t WHERE a <> 10");
        auto* cmp = static_cast<BinaryExpr*>(result.stmt->whereClause);
        REQUIRE(cmp->op == BinaryOp::NE);
    }
}

TEST_CASE("ASTContext arena ownership", "[parser]") {
    ASTContext ctx;
    auto* lit = ctx.alloc<Literal>();
    lit->literalType = LiteralType::INTEGER;
    lit->intValue = 42;

    auto* ref = ctx.alloc<ColumnRef>();
    ref->columnName = "test_col";

    auto* tref = ctx.allocTableRef();
    tref->tableName = "test_table";

    auto* stmt = ctx.allocSelectStmt();
    stmt->selectList.push_back(lit);
    stmt->fromTable = tref;

    REQUIRE(lit->intValue == 42);
    REQUIRE(ref->columnName == "test_col");
    REQUIRE(tref->tableName == "test_table");
    REQUIRE(stmt->selectList.size() == 1);
}

TEST_CASE("ParseError has cursor position", "[parser]") {
    try {
        Parser::parse("SELEC x FROM t");
        REQUIRE(false);  // Should not reach here
    } catch (const ParseError& e) {
        REQUIRE(e.cursorPos() > 0);
        std::string msg = e.what();
        REQUIRE(!msg.empty());
    }
}

TEST_CASE("Boolean AND in WHERE clause", "[parser]") {
    auto result = Parser::parse("SELECT a FROM t WHERE a > 1 AND b < 2");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->whereClause != nullptr);
    REQUIRE(result.stmt->whereClause->kind == ExprKind::BINARY_EXPR);
    auto* andExpr = static_cast<BinaryExpr*>(result.stmt->whereClause);
    REQUIRE(andExpr->op == BinaryOp::AND);
}

TEST_CASE("GROUP BY with expressions", "[parser]") {
    auto result = Parser::parse("SELECT a, COUNT(*) FROM t GROUP BY a");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->groupBy.size() == 1);
    REQUIRE(result.stmt->groupBy[0]->kind == ExprKind::COLUMN_REF);
}

TEST_CASE("Arithmetic expressions", "[parser]") {
    auto result = Parser::parse("SELECT a + b, a * b, a - b, a / b FROM t");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->selectList.size() == 4);

    // a + b
    auto* ae0 = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae0->expr->kind == ExprKind::BINARY_EXPR);
    auto* bin0 = static_cast<BinaryExpr*>(ae0->expr);
    REQUIRE(bin0->op == BinaryOp::ADD);

    // a * b
    auto* ae1 = static_cast<AliasedExpr*>(result.stmt->selectList[1]);
    auto* bin1 = static_cast<BinaryExpr*>(ae1->expr);
    REQUIRE(bin1->op == BinaryOp::MUL);

    // a - b
    auto* ae2 = static_cast<AliasedExpr*>(result.stmt->selectList[2]);
    auto* bin2 = static_cast<BinaryExpr*>(ae2->expr);
    REQUIRE(bin2->op == BinaryOp::SUB);

    // a / b
    auto* ae3 = static_cast<AliasedExpr*>(result.stmt->selectList[3]);
    auto* bin3 = static_cast<BinaryExpr*>(ae3->expr);
    REQUIRE(bin3->op == BinaryOp::DIV);
}

TEST_CASE("AST printer null expr", "[parser]") {
    std::string printed = ASTPrinter::printExpr(nullptr);
    REQUIRE(printed.find("null") != std::string::npos);
}

TEST_CASE("Table alias", "[parser]") {
    auto result = Parser::parse("SELECT a FROM t AS alias1");
    REQUIRE(result.stmt != nullptr);
    REQUIRE(result.stmt->fromTable != nullptr);
    REQUIRE(result.stmt->fromTable->tableName == "t");
    REQUIRE(result.stmt->fromTable->alias == "alias1");
}

TEST_CASE("Cast expression", "[parser]") {
    auto result = Parser::parse("SELECT CAST(a AS int) FROM t");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    // PG may represent CAST in different ways; verify we get some expression
    REQUIRE(ae->expr != nullptr);
}

TEST_CASE("Unary negation", "[parser]") {
    auto result = Parser::parse("SELECT -a FROM t");
    REQUIRE(result.stmt != nullptr);
    auto* ae = static_cast<AliasedExpr*>(result.stmt->selectList[0]);
    REQUIRE(ae->expr->kind == ExprKind::UNARY_EXPR);
    auto* un = static_cast<UnaryExpr*>(ae->expr);
    REQUIRE(un->op == UnaryOp::NEG);
    REQUIRE(un->operand->kind == ExprKind::COLUMN_REF);
}
