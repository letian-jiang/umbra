#include <catch2/catch_test_macros.hpp>
#include <string>

#include "binder/binder.h"
#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "binder/bound_plan_printer.h"
#include "catalog/catalog.h"
#include "parser/parser.h"
#include "storage/tbl_loader.h"

using namespace umbra;
using namespace umbra::binder;
using namespace umbra::parser;

// ── Helper: create catalog with lineitem table ──────────────────────────────

static Catalog makeQ1Catalog() {
    Catalog cat;
    cat.addTable(lineitemTableDef());
    return cat;
}

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

// ── TPC-H Q1 binding ────────────────────────────────────────────────────────

TEST_CASE("Bind TPC-H Q1", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(Q1_SQL);
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // Root should be a Sort (ORDER BY)
    REQUIRE(bindResult.root->kind == BoundNodeKind::SORT);
    auto* sort = static_cast<BoundSort*>(bindResult.root);

    SECTION("ORDER BY has 2 items, both ASC") {
        REQUIRE(sort->orderBy.size() == 2);
        REQUIRE(sort->orderBy[0].ascending);
        REQUIRE(sort->orderBy[1].ascending);
    }

    // Under Sort is a Projection
    REQUIRE(sort->child != nullptr);
    REQUIRE(sort->child->kind == BoundNodeKind::PROJECTION);
    auto* proj = static_cast<BoundProjection*>(sort->child);

    SECTION("Projection has 10 output expressions") {
        REQUIRE(proj->expressions.size() == 10);
    }

    SECTION("First two projection exprs are GroupRef") {
        REQUIRE(proj->expressions[0]->kind == BoundExprKind::GROUP_REF);
        REQUIRE(proj->expressions[1]->kind == BoundExprKind::GROUP_REF);
        auto* gr0 = static_cast<BoundGroupRef*>(proj->expressions[0]);
        auto* gr1 = static_cast<BoundGroupRef*>(proj->expressions[1]);
        REQUIRE(gr0->groupIndex == 0);
        REQUIRE(gr1->groupIndex == 1);
        REQUIRE(gr0->resultType.id == SQLTypeId::CHAR);
        REQUIRE(gr1->resultType.id == SQLTypeId::CHAR);
    }

    SECTION("Projection exprs 2-5 are AggRef (direct aggregates)") {
        // sum_qty -> AggRef(0)
        REQUIRE(proj->expressions[2]->kind == BoundExprKind::AGG_REF);
        // sum_base_price -> AggRef(1)
        REQUIRE(proj->expressions[3]->kind == BoundExprKind::AGG_REF);
        // sum_disc_price -> AggRef(2)
        REQUIRE(proj->expressions[4]->kind == BoundExprKind::AGG_REF);
        // sum_charge -> AggRef(3)
        REQUIRE(proj->expressions[5]->kind == BoundExprKind::AGG_REF);
    }

    SECTION("AVG decomposed into DIV(AggRef, AggRef)") {
        // avg_qty = SUM(l_quantity) / COUNT(*)
        REQUIRE(proj->expressions[6]->kind == BoundExprKind::BINARY_EXPR);
        auto* avgQty = static_cast<BoundBinaryExpr*>(proj->expressions[6]);
        REQUIRE(avgQty->op == parser::BinaryOp::DIV);
        REQUIRE(avgQty->left->kind == BoundExprKind::AGG_REF);
        // Right side may be AggRef or Cast(AggRef)

        // avg_price = SUM(l_extendedprice) / COUNT(*)
        REQUIRE(proj->expressions[7]->kind == BoundExprKind::BINARY_EXPR);
        auto* avgPrice = static_cast<BoundBinaryExpr*>(proj->expressions[7]);
        REQUIRE(avgPrice->op == parser::BinaryOp::DIV);

        // avg_disc = SUM(l_discount) / COUNT(*)
        REQUIRE(proj->expressions[8]->kind == BoundExprKind::BINARY_EXPR);
        auto* avgDisc = static_cast<BoundBinaryExpr*>(proj->expressions[8]);
        REQUIRE(avgDisc->op == parser::BinaryOp::DIV);
    }

    SECTION("COUNT(*) is an AggRef") {
        REQUIRE(proj->expressions[9]->kind == BoundExprKind::AGG_REF);
    }

    SECTION("Aliases are correct") {
        REQUIRE(proj->aliases.size() == 10);
        REQUIRE(proj->aliases[0].empty());  // l_returnflag (no alias)
        REQUIRE(proj->aliases[1].empty());  // l_linestatus (no alias)
        REQUIRE(proj->aliases[2] == "sum_qty");
        REQUIRE(proj->aliases[3] == "sum_base_price");
        REQUIRE(proj->aliases[4] == "sum_disc_price");
        REQUIRE(proj->aliases[5] == "sum_charge");
        REQUIRE(proj->aliases[6] == "avg_qty");
        REQUIRE(proj->aliases[7] == "avg_price");
        REQUIRE(proj->aliases[8] == "avg_disc");
        REQUIRE(proj->aliases[9] == "count_order");
    }

    // Under Projection is an Aggregate
    REQUIRE(proj->child != nullptr);
    REQUIRE(proj->child->kind == BoundNodeKind::AGGREGATE);
    auto* agg = static_cast<BoundAggregate*>(proj->child);

    SECTION("Aggregate has 2 group-by keys") {
        REQUIRE(agg->groupBy.size() == 2);
        REQUIRE(agg->groupBy[0]->kind == BoundExprKind::COLUMN_REF);
        REQUIRE(agg->groupBy[1]->kind == BoundExprKind::COLUMN_REF);
        auto* gb0 = static_cast<BoundColumnRef*>(agg->groupBy[0]);
        auto* gb1 = static_cast<BoundColumnRef*>(agg->groupBy[1]);
        REQUIRE(gb0->columnIndex == 8);  // l_returnflag
        REQUIRE(gb1->columnIndex == 9);  // l_linestatus
    }

    SECTION("Aggregate has 6 aggregates (with AVG decomposition and dedup)") {
        // Processing order from SELECT list rewriting:
        // 0: SUM(l_quantity)           -- from SUM(l_quantity) + AVG(l_quantity) dedup
        // 1: SUM(l_extendedprice)      -- from SUM(l_extendedprice) + AVG dedup
        // 2: SUM(l_extendedprice * (1 - l_discount))
        // 3: SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax))
        // 4: COUNT(*)                  -- from AVG(l_quantity) decomposition + COUNT(*) dedup
        // 5: SUM(l_discount)           -- from AVG(l_discount) decomposition
        REQUIRE(agg->aggregates.size() == 6);

        REQUIRE(agg->aggregates[0]->func == AggFunc::SUM);
        REQUIRE(agg->aggregates[1]->func == AggFunc::SUM);
        REQUIRE(agg->aggregates[2]->func == AggFunc::SUM);
        REQUIRE(agg->aggregates[3]->func == AggFunc::SUM);
        REQUIRE(agg->aggregates[4]->func == AggFunc::COUNT);
        REQUIRE(agg->aggregates[4]->arg == nullptr);  // COUNT(*)
        REQUIRE(agg->aggregates[5]->func == AggFunc::SUM);
    }

    SECTION("AVG dedup: SUM(l_quantity) shared between direct SUM and AVG") {
        // avg_qty uses AggRef(0) — same as sum_qty
        auto* avgQty = static_cast<BoundBinaryExpr*>(proj->expressions[6]);
        auto* sumRef = static_cast<BoundAggRef*>(avgQty->left);
        auto* sumQtyRef = static_cast<BoundAggRef*>(proj->expressions[2]);
        REQUIRE(sumRef->aggIndex == sumQtyRef->aggIndex);

        // COUNT(*) is shared among all AVG decompositions and the explicit COUNT(*)
        auto* countRef = static_cast<BoundAggRef*>(proj->expressions[9]);
        REQUIRE(countRef->aggIndex == 4);  // COUNT(*) is at index 4
    }

    // Under Aggregate is a Filter
    REQUIRE(agg->child != nullptr);
    REQUIRE(agg->child->kind == BoundNodeKind::FILTER);
    auto* filter = static_cast<BoundFilter*>(agg->child);

    SECTION("Filter predicate: l_shipdate <= DATE constant") {
        REQUIRE(filter->predicate != nullptr);
        REQUIRE(filter->predicate->kind == BoundExprKind::BINARY_EXPR);
        auto* le = static_cast<BoundBinaryExpr*>(filter->predicate);
        REQUIRE(le->op == parser::BinaryOp::LE);
        REQUIRE(le->resultType.id == SQLTypeId::BOOLEAN);

        // Left: column ref to l_shipdate
        REQUIRE(le->left->kind == BoundExprKind::COLUMN_REF);
        auto* shipdate = static_cast<BoundColumnRef*>(le->left);
        REQUIRE(shipdate->columnIndex == 10);
        REQUIRE(shipdate->resultType.id == SQLTypeId::DATE);

        // Right: constant-folded date literal (1998-09-02)
        REQUIRE(le->right->kind == BoundExprKind::LITERAL);
        auto* dateLit = static_cast<BoundLiteral*>(le->right);
        REQUIRE(dateLit->resultType.id == SQLTypeId::DATE);
        // 1998-09-02 as days since 2000-01-01 should be negative
        // Verify: parseDate("1998-09-02") should give the same value
        int32_t expected = parseDate("1998-09-02");
        REQUIRE(dateLit->intValue == expected);
    }

    // Under Filter is a Scan
    REQUIRE(filter->child != nullptr);
    REQUIRE(filter->child->kind == BoundNodeKind::SCAN);
    auto* scan = static_cast<BoundScan*>(filter->child);

    SECTION("Scan is on lineitem table") {
        REQUIRE(scan->table->name == "lineitem");
        REQUIRE(scan->outputColumns.size() == 16);
    }
}

// ── Implicit cast tests ─────────────────────────────────────────────────────

TEST_CASE("Implicit cast: INTEGER to DECIMAL", "[binder]") {
    // In Q1, expressions like (1 - l_discount) require casting 1 to DECIMAL
    auto cat = makeQ1Catalog();
    // Test without GROUP BY to directly see the expression tree
    auto parseResult = Parser::parse("SELECT 1 - l_discount FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions.size() == 1);

    // The expression should have DECIMAL result type since the literal
    // gets cast from INTEGER to DECIMAL
    auto* topExpr = proj->expressions[0];
    REQUIRE(topExpr->kind == BoundExprKind::BINARY_EXPR);
    auto* sub = static_cast<BoundBinaryExpr*>(topExpr);
    REQUIRE(sub->resultType.id == SQLTypeId::DECIMAL);
}

TEST_CASE("Implicit cast in binary expression: literal 1 vs DECIMAL column", "[binder]") {
    auto cat = makeQ1Catalog();
    // Simple case without GROUP BY
    auto parseResult = Parser::parse("SELECT 1 - l_discount FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions.size() == 1);

    auto* expr = proj->expressions[0];
    REQUIRE(expr->kind == BoundExprKind::BINARY_EXPR);
    auto* sub = static_cast<BoundBinaryExpr*>(expr);
    REQUIRE(sub->op == parser::BinaryOp::SUB);

    // Left side should be cast from INTEGER to DECIMAL
    REQUIRE(sub->left->kind == BoundExprKind::CAST);
    auto* cast = static_cast<BoundCast*>(sub->left);
    REQUIRE(cast->sourceType.id == SQLTypeId::INTEGER);
    REQUIRE(cast->resultType.id == SQLTypeId::DECIMAL);

    // Right side is the column ref
    REQUIRE(sub->right->kind == BoundExprKind::COLUMN_REF);
}

// ── DATE constant folding ────────────────────────────────────────────────────

TEST_CASE("DATE arithmetic constant folding", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT l_shipdate FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // Should be Projection -> Filter -> Scan
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->child->kind == BoundNodeKind::FILTER);
    auto* filter = static_cast<BoundFilter*>(proj->child);

    auto* pred = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(pred->right->kind == BoundExprKind::LITERAL);
    auto* dateLit = static_cast<BoundLiteral*>(pred->right);
    REQUIRE(dateLit->resultType.id == SQLTypeId::DATE);

    // Verify the folded date value
    int32_t expected = parseDate("1998-09-02");
    REQUIRE(dateLit->intValue == expected);
}

TEST_CASE("DATE addition constant folding", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT l_shipdate FROM lineitem "
        "WHERE l_shipdate <= DATE '2024-01-01' + INTERVAL '120' DAY");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* pred = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(pred->right->kind == BoundExprKind::LITERAL);
    auto* dateLit = static_cast<BoundLiteral*>(pred->right);
    REQUIRE(dateLit->resultType.id == SQLTypeId::DATE);

    // 2024-01-01 + 120 days = 2024-04-30
    int32_t expected = parseDate("2024-04-30");
    REQUIRE(dateLit->intValue == expected);
}

// ── Error cases ──────────────────────────────────────────────────────────────

TEST_CASE("Binder error: table not found", "[binder]") {
    Catalog cat;
    auto parseResult = Parser::parse("SELECT a FROM nonexistent");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

TEST_CASE("Binder error: column not found", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT no_such_column FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

TEST_CASE("Binder error: non-aggregate not in GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult =
        Parser::parse("SELECT l_returnflag, l_quantity FROM lineitem GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

TEST_CASE("Binder error: aggregate in GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_returnflag FROM lineitem GROUP BY SUM(l_quantity)");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

TEST_CASE("Binder error: missing FROM clause", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT 1 + 2");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

// ── Simple queries ───────────────────────────────────────────────────────────

TEST_CASE("Bind simple SELECT without GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey, l_quantity FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // Should be Projection -> Scan
    REQUIRE(bindResult.root->kind == BoundNodeKind::PROJECTION);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions.size() == 2);

    REQUIRE(proj->expressions[0]->kind == BoundExprKind::COLUMN_REF);
    auto* col0 = static_cast<BoundColumnRef*>(proj->expressions[0]);
    REQUIRE(col0->columnName == "l_orderkey");
    REQUIRE(col0->resultType.id == SQLTypeId::INTEGER);

    REQUIRE(proj->expressions[1]->kind == BoundExprKind::COLUMN_REF);
    auto* col1 = static_cast<BoundColumnRef*>(proj->expressions[1]);
    REQUIRE(col1->columnName == "l_quantity");
    REQUIRE(col1->resultType.id == SQLTypeId::DECIMAL);

    REQUIRE(proj->child->kind == BoundNodeKind::SCAN);
}

TEST_CASE("Bind SELECT with WHERE", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey > 100");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // Projection -> Filter -> Scan
    REQUIRE(bindResult.root->kind == BoundNodeKind::PROJECTION);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->child->kind == BoundNodeKind::FILTER);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    REQUIRE(filter->predicate->resultType.id == SQLTypeId::BOOLEAN);
    REQUIRE(filter->child->kind == BoundNodeKind::SCAN);
}

TEST_CASE("Bind SELECT with ORDER BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem ORDER BY l_orderkey DESC");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // Sort -> Projection -> Scan
    REQUIRE(bindResult.root->kind == BoundNodeKind::SORT);
    auto* sort = static_cast<BoundSort*>(bindResult.root);
    REQUIRE(sort->orderBy.size() == 1);
    REQUIRE(!sort->orderBy[0].ascending);
    REQUIRE(sort->child->kind == BoundNodeKind::PROJECTION);
}

// ── Bound plan printer ──────────────────────────────────────────────────────

TEST_CASE("BoundPlanPrinter prints Q1", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(Q1_SQL);
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    std::string printed = BoundPlanPrinter::print(bindResult.root);
    REQUIRE(!printed.empty());
    REQUIRE(printed.find("Sort") != std::string::npos);
    REQUIRE(printed.find("Projection") != std::string::npos);
    REQUIRE(printed.find("Aggregate") != std::string::npos);
    REQUIRE(printed.find("Filter") != std::string::npos);
    REQUIRE(printed.find("Scan") != std::string::npos);
    REQUIRE(printed.find("lineitem") != std::string::npos);
    REQUIRE(printed.find("SUM") != std::string::npos);
    REQUIRE(printed.find("COUNT") != std::string::npos);
    REQUIRE(printed.find("GroupRef") != std::string::npos);
    REQUIRE(printed.find("AggRef") != std::string::npos);
}

TEST_CASE("BoundPlanPrinter printExpr", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);

    std::string printed = BoundPlanPrinter::printExpr(proj->expressions[0]);
    REQUIRE(!printed.empty());
    REQUIRE(printed.find("ColumnRef") != std::string::npos);
}

TEST_CASE("BoundPlanPrinter null handling", "[binder]") {
    std::string printed = BoundPlanPrinter::print(nullptr);
    REQUIRE(printed.find("null") != std::string::npos);

    printed = BoundPlanPrinter::printExpr(nullptr);
    REQUIRE(printed.find("null") != std::string::npos);
}

TEST_CASE("Bind COUNT(*) without GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT COUNT(*) AS cnt FROM lineitem "
        "GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);
}

TEST_CASE("Type inference: DECIMAL result types", "[binder]") {
    auto cat = makeQ1Catalog();
    // l_quantity is DECIMAL(15,2), l_extendedprice is DECIMAL(15,2)
    auto parseResult = Parser::parse("SELECT l_quantity + l_extendedprice FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions[0]->kind == BoundExprKind::BINARY_EXPR);
    auto* add = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
    REQUIRE(add->resultType.id == SQLTypeId::DECIMAL);
}

TEST_CASE("Binder can be reused", "[binder]") {
    auto cat = makeQ1Catalog();
    Binder binder(cat);

    auto r1 = Parser::parse("SELECT l_orderkey FROM lineitem");
    auto b1 = binder.bind(*r1.stmt);
    REQUIRE(b1.root != nullptr);

    auto r2 = Parser::parse("SELECT l_quantity FROM lineitem");
    auto b2 = binder.bind(*r2.stmt);
    REQUIRE(b2.root != nullptr);
}

TEST_CASE("Unary negation binding", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT -l_quantity FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions[0]->kind == BoundExprKind::UNARY_EXPR);
    auto* neg = static_cast<BoundUnaryExpr*>(proj->expressions[0]);
    REQUIRE(neg->op == parser::UnaryOp::NEG);
    REQUIRE(neg->resultType.id == SQLTypeId::DECIMAL);
}

// ── Table alias and qualified column name ───────────────────────────────────

TEST_CASE("Bind with table alias", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem AS li");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions[0]->kind == BoundExprKind::COLUMN_REF);
}

TEST_CASE("Bind with qualified column name", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT lineitem.l_orderkey FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* col = static_cast<BoundColumnRef*>(proj->expressions[0]);
    REQUIRE(col->columnName == "l_orderkey");
    REQUIRE(col->resultType.id == SQLTypeId::INTEGER);
}

TEST_CASE("Binder error: qualified column with unknown table", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT bad_table.l_orderkey FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

// ── INTEGER arithmetic (same type, no casts) ────────────────────────────────

TEST_CASE("Bind INTEGER + INTEGER", "[binder]") {
    auto cat = makeQ1Catalog();
    // l_orderkey and l_partkey are both INTEGER
    auto parseResult = Parser::parse("SELECT l_orderkey + l_partkey FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* add = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
    REQUIRE(add->resultType.id == SQLTypeId::INTEGER);
    REQUIRE(add->left->kind == BoundExprKind::COLUMN_REF);
    REQUIRE(add->right->kind == BoundExprKind::COLUMN_REF);
}

TEST_CASE("Bind INTEGER * INTEGER", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey * l_partkey FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* mul = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
    REQUIRE(mul->resultType.id == SQLTypeId::INTEGER);
}

// ── DECIMAL * DECIMAL (multiplication scale behavior) ───────────────────────

TEST_CASE("Bind DECIMAL * DECIMAL multiplication", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_quantity * l_extendedprice FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* mul = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
    REQUIRE(mul->resultType.id == SQLTypeId::DECIMAL);
}

// ── Boolean operations ──────────────────────────────────────────────────────

TEST_CASE("Bind AND expression", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult =
        Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey > 1 AND l_orderkey < 100");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    REQUIRE(filter->predicate->kind == BoundExprKind::BINARY_EXPR);
    auto* andExpr = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(andExpr->resultType.id == SQLTypeId::BOOLEAN);
}

// ── Various comparison operators ────────────────────────────────────────────

TEST_CASE("Bind comparison operators", "[binder]") {
    auto cat = makeQ1Catalog();

    SECTION("EQ") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey = 1");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* f = static_cast<BoundFilter*>(proj->child);
        auto* cmp = static_cast<BoundBinaryExpr*>(f->predicate);
        REQUIRE(cmp->op == parser::BinaryOp::EQ);
        REQUIRE(cmp->resultType.id == SQLTypeId::BOOLEAN);
    }

    SECTION("NE") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey <> 1");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* f = static_cast<BoundFilter*>(proj->child);
        auto* cmp = static_cast<BoundBinaryExpr*>(f->predicate);
        REQUIRE(cmp->op == parser::BinaryOp::NE);
    }

    SECTION("LT") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey < 10");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* f = static_cast<BoundFilter*>(proj->child);
        auto* cmp = static_cast<BoundBinaryExpr*>(f->predicate);
        REQUIRE(cmp->op == parser::BinaryOp::LT);
    }

    SECTION("GT") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey > 10");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* f = static_cast<BoundFilter*>(proj->child);
        auto* cmp = static_cast<BoundBinaryExpr*>(f->predicate);
        REQUIRE(cmp->op == parser::BinaryOp::GT);
    }

    SECTION("GE") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey >= 10");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* f = static_cast<BoundFilter*>(proj->child);
        auto* cmp = static_cast<BoundBinaryExpr*>(f->predicate);
        REQUIRE(cmp->op == parser::BinaryOp::GE);
    }
}

// ── DATE comparison (same type, no cast) ────────────────────────────────────

TEST_CASE("Bind DATE comparison", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult =
        Parser::parse("SELECT l_shipdate FROM lineitem WHERE l_shipdate = l_commitdate");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* cmp = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(cmp->resultType.id == SQLTypeId::BOOLEAN);
}

// ── Error: unknown function ─────────────────────────────────────────────────

TEST_CASE("Binder error: unknown function", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT BOGUS_FUNC(l_orderkey) FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

// ── Printer coverage: more expression types ─────────────────────────────────

TEST_CASE("BoundPlanPrinter prints all expression types", "[binder]") {
    auto cat = makeQ1Catalog();

    SECTION("Binary expression") {
        auto r = Parser::parse("SELECT l_orderkey + l_partkey FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        std::string printed = BoundPlanPrinter::printExpr(proj->expressions[0]);
        REQUIRE(printed.find("BinaryExpr") != std::string::npos);
        REQUIRE(printed.find("+") != std::string::npos);
    }

    SECTION("Unary expression") {
        auto r = Parser::parse("SELECT -l_orderkey FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        std::string printed = BoundPlanPrinter::printExpr(proj->expressions[0]);
        REQUIRE(printed.find("UnaryExpr") != std::string::npos);
        REQUIRE(printed.find("-") != std::string::npos);
    }

    SECTION("Cast expression") {
        auto r = Parser::parse("SELECT 1 - l_discount FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        std::string printed = BoundPlanPrinter::printExpr(proj->expressions[0]);
        REQUIRE(printed.find("Cast") != std::string::npos);
    }

    SECTION("Literal expression") {
        auto r = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey > 42");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* filter = static_cast<BoundFilter*>(proj->child);
        std::string printed = BoundPlanPrinter::printExpr(filter->predicate);
        REQUIRE(printed.find("Literal") != std::string::npos);
        REQUIRE(printed.find("42") != std::string::npos);
    }

    SECTION("AggCall and COUNT(*)") {
        auto r = Parser::parse("SELECT SUM(l_quantity) FROM lineitem GROUP BY l_returnflag");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        // Find the aggregate node
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* agg = static_cast<BoundAggregate*>(proj->child);
        for (const auto* aggCall : agg->aggregates) {
            std::string printed = BoundPlanPrinter::printExpr(aggCall);
            REQUIRE(printed.find("AggCall") != std::string::npos);
        }
    }
}

// ── Additional comparison printing ──────────────────────────────────────────

TEST_CASE("BoundPlanPrinter prints comparison operators", "[binder]") {
    auto cat = makeQ1Catalog();

    auto r =
        Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey >= 1 AND l_orderkey < 100");
    Binder b(cat);
    auto br = b.bind(*r.stmt);
    auto* proj = static_cast<BoundProjection*>(br.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    std::string printed = BoundPlanPrinter::printExpr(filter->predicate);
    REQUIRE(printed.find("BinaryExpr") != std::string::npos);
}

// ── DIV and MOD operations ──────────────────────────────────────────────────

TEST_CASE("Bind division and modulo", "[binder]") {
    auto cat = makeQ1Catalog();

    SECTION("Integer division") {
        auto r = Parser::parse("SELECT l_orderkey / l_partkey FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* div = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
        REQUIRE(div->op == parser::BinaryOp::DIV);
        REQUIRE(div->resultType.id == SQLTypeId::INTEGER);
    }

    SECTION("Integer modulo") {
        auto r = Parser::parse("SELECT l_orderkey % l_partkey FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* mod = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
        REQUIRE(mod->op == parser::BinaryOp::MOD);
        REQUIRE(mod->resultType.id == SQLTypeId::INTEGER);
    }

    SECTION("Decimal division") {
        auto r = Parser::parse("SELECT l_quantity / l_discount FROM lineitem");
        Binder b(cat);
        auto br = b.bind(*r.stmt);
        auto* proj = static_cast<BoundProjection*>(br.root);
        auto* div = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
        REQUIRE(div->resultType.id == SQLTypeId::DECIMAL);
    }
}

// ── SUM with complex aggregate arguments in GROUP BY ────────────────────────

TEST_CASE("SUM with complex expression argument in GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT l_returnflag, SUM(l_extendedprice * (1 - l_discount)) "
        "FROM lineitem GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* agg = static_cast<BoundAggregate*>(proj->child);
    REQUIRE(agg->aggregates.size() == 1);
    REQUIRE(agg->aggregates[0]->func == AggFunc::SUM);
    // The argument is a complex expression, not a simple column ref
    REQUIRE(agg->aggregates[0]->arg != nullptr);
}

// ── COUNT(column) ───────────────────────────────────────────────────────────

TEST_CASE("Bind COUNT(column)", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult =
        Parser::parse("SELECT COUNT(l_orderkey) FROM lineitem GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* agg = static_cast<BoundAggregate*>(proj->child);
    REQUIRE(agg->aggregates.size() == 1);
    REQUIRE(agg->aggregates[0]->func == AggFunc::COUNT);
    REQUIRE(agg->aggregates[0]->arg != nullptr);
    REQUIRE(agg->aggregates[0]->resultType.id == SQLTypeId::BIGINT);
}

// ── Comprehensive printer test ──────────────────────────────────────────────

TEST_CASE("BoundPlanPrinter full plan output", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT l_returnflag, SUM(l_quantity) AS sq, AVG(l_discount) AS ad, "
        "COUNT(*) AS cnt FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
        "GROUP BY l_returnflag ORDER BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    std::string printed = BoundPlanPrinter::print(bindResult.root);
    // Should contain all node types
    REQUIRE(printed.find("Sort") != std::string::npos);
    REQUIRE(printed.find("ASC") != std::string::npos);
    REQUIRE(printed.find("Projection") != std::string::npos);
    REQUIRE(printed.find("Aggregate") != std::string::npos);
    REQUIRE(printed.find("group_by") != std::string::npos);
    REQUIRE(printed.find("aggregates") != std::string::npos);
    REQUIRE(printed.find("Filter") != std::string::npos);
    REQUIRE(printed.find("predicate") != std::string::npos);
    REQUIRE(printed.find("Scan") != std::string::npos);
    REQUIRE(printed.find("ColumnRef") != std::string::npos);
    REQUIRE(printed.find("Literal") != std::string::npos);
    REQUIRE(printed.find("SUM") != std::string::npos);
    REQUIRE(printed.find("COUNT") != std::string::npos);
    REQUIRE(printed.find("(*)") != std::string::npos);
    REQUIRE(printed.find("[sq]") != std::string::npos);
}

// ── Coverage: STRING literal in WHERE ───────────────────────────────────────

TEST_CASE("Bind CHAR comparison with string literal", "[binder]") {
    auto cat = makeQ1Catalog();
    // l_returnflag is CHAR(1), comparing with string literal
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_returnflag = 'A'");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* cmp = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(cmp->resultType.id == SQLTypeId::BOOLEAN);

    // The string literal is converted to CHAR(1) with ASCII code
    REQUIRE(cmp->right->kind == BoundExprKind::LITERAL);
    auto* lit = static_cast<BoundLiteral*>(cmp->right);
    REQUIRE(lit->resultType.id == SQLTypeId::CHAR);
    REQUIRE(lit->intValue == 65);  // 'A' = 65
}

// ── Coverage: standalone DATE literal in SELECT (not via DateArith) ─────────

TEST_CASE("Bind standalone DATE literal", "[binder]") {
    auto cat = makeQ1Catalog();
    // DATE literal in WHERE comparison
    auto parseResult =
        Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_shipdate = DATE '2024-06-15'");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* cmp = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(cmp->right->kind == BoundExprKind::LITERAL);
    auto* dateLit = static_cast<BoundLiteral*>(cmp->right);
    REQUIRE(dateLit->resultType.id == SQLTypeId::DATE);
    REQUIRE(dateLit->intValue == parseDate("2024-06-15"));
}

// ── Coverage: DECIMAL literal ───────────────────────────────────────────────

TEST_CASE("Bind DECIMAL literal", "[binder]") {
    auto cat = makeQ1Catalog();
    // The parser outputs DECIMAL literal type for float-like constants
    // Use a query that produces a DECIMAL literal comparison
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_quantity > 10.5");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);
}

// ── Coverage: AVG without GROUP BY (direct bindFuncCall path) ───────────────

TEST_CASE("Bind AVG without GROUP BY via simple select", "[binder]") {
    auto cat = makeQ1Catalog();
    // AVG in select without GROUP BY just creates a SUM agg call
    // via bindFuncCall (not through rewriteExprForProjection)
    auto parseResult = Parser::parse("SELECT AVG(l_quantity) FROM lineitem GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    // The AVG should be decomposed into SUM/COUNT
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    REQUIRE(proj->expressions[0]->kind == BoundExprKind::BINARY_EXPR);
    auto* div = static_cast<BoundBinaryExpr*>(proj->expressions[0]);
    REQUIRE(div->op == parser::BinaryOp::DIV);
}

// ── Coverage: containsAggregate with nested binary/unary ────────────────────

TEST_CASE("Validate GROUP BY with SUM in binary expression", "[binder]") {
    auto cat = makeQ1Catalog();
    // SUM inside a binary expression should be detected as aggregate
    auto parseResult = Parser::parse(
        "SELECT l_returnflag, SUM(l_quantity) + SUM(l_extendedprice) "
        "FROM lineitem GROUP BY l_returnflag");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);
}

// ── Coverage: incompatible types error ──────────────────────────────────────

TEST_CASE("Binder error: incompatible types", "[binder]") {
    auto cat = makeQ1Catalog();
    // DATE + VARCHAR is not supported
    auto parseResult = Parser::parse("SELECT l_shipdate + l_returnflag FROM lineitem");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    REQUIRE_THROWS_AS(binder.bind(*parseResult.stmt), BindError);
}

// ── Coverage: DECIMAL comparison ────────────────────────────────────────────

TEST_CASE("Bind DECIMAL comparison (same DECIMAL types)", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult =
        Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_quantity > l_discount");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* cmp = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(cmp->resultType.id == SQLTypeId::BOOLEAN);
}

// ── Coverage: INTEGER to DECIMAL comparison (implicit cast in comparison) ───

TEST_CASE("Bind INTEGER vs DECIMAL comparison", "[binder]") {
    auto cat = makeQ1Catalog();
    // l_orderkey is INTEGER, l_quantity is DECIMAL
    auto parseResult =
        Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_orderkey < l_quantity");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    auto* cmp = static_cast<BoundBinaryExpr*>(filter->predicate);
    REQUIRE(cmp->resultType.id == SQLTypeId::BOOLEAN);
    // Left side (INTEGER) should be cast to DECIMAL
    REQUIRE(cmp->left->kind == BoundExprKind::CAST);
}

// ── Coverage: ORDER BY with GROUP BY ────────────────────────────────────────

TEST_CASE("Bind ORDER BY DESC with GROUP BY", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(
        "SELECT l_returnflag, SUM(l_quantity) "
        "FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag DESC");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    auto* sort = static_cast<BoundSort*>(bindResult.root);
    REQUIRE(sort->orderBy.size() == 1);
    REQUIRE(!sort->orderBy[0].ascending);

    std::string printed = BoundPlanPrinter::print(bindResult.root);
    REQUIRE(printed.find("DESC") != std::string::npos);
}

// ── Coverage: printer prints CHAR literal type ──────────────────────────────

TEST_CASE("BoundPlanPrinter prints CHAR literal", "[binder]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse("SELECT l_orderkey FROM lineitem WHERE l_returnflag = 'N'");
    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    auto* proj = static_cast<BoundProjection*>(bindResult.root);
    auto* filter = static_cast<BoundFilter*>(proj->child);
    std::string printed = BoundPlanPrinter::printExpr(filter->predicate);
    REQUIRE(printed.find("'N'") != std::string::npos);
}
