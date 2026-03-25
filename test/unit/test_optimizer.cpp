#include <catch2/catch_test_macros.hpp>
#include <set>
#include <string>

#include "binder/binder.h"
#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "catalog/catalog.h"
#include "optimizer/optimizer.h"
#include "optimizer/physical_operator.h"
#include "optimizer/physical_plan_printer.h"
#include "parser/parser.h"
#include "storage/tbl_loader.h"

using namespace umbra;
using namespace umbra::binder;
using namespace umbra::optimizer;
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

// ── TPC-H Q1 optimization ──────────────────────────────────────────────────

TEST_CASE("Optimize TPC-H Q1 — physical plan structure", "[optimizer]") {
    // Binder must stay alive — it owns bound plan nodes.
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(Q1_SQL);
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    Optimizer opt;
    auto result = opt.optimize(bindResult);
    REQUIRE(result.root != nullptr);

    // Root should be PhysicalSort
    REQUIRE(result.root->kind == PhysicalNodeKind::SORT);
    auto* sort = static_cast<PhysicalSort*>(result.root);

    SECTION("Sort has 2 order-by items, both ASC") {
        REQUIRE(sort->orderBy.size() == 2);
        REQUIRE(sort->orderBy[0].ascending);
        REQUIRE(sort->orderBy[1].ascending);
    }

    // Child of Sort should be PhysicalProjection
    REQUIRE(sort->child != nullptr);
    REQUIRE(sort->child->kind == PhysicalNodeKind::PROJECTION);
    auto* proj = static_cast<PhysicalProjection*>(sort->child);

    SECTION("Projection has 10 output expressions") {
        REQUIRE(proj->expressions.size() == 10);
    }

    // Child of Projection should be PhysicalHashAggregate
    REQUIRE(proj->child != nullptr);
    REQUIRE(proj->child->kind == PhysicalNodeKind::HASH_AGGREGATE);
    auto* agg = static_cast<PhysicalHashAggregate*>(proj->child);

    SECTION("HashAggregate has 2 group-by keys") {
        REQUIRE(agg->groupByKeys.size() == 2);
    }

    SECTION("HashAggregate has correct number of aggregates") {
        // Q1: SUM(qty), SUM(extprice), SUM(disc_price), SUM(charge),
        //     SUM(qty), SUM(extprice), SUM(discount), COUNT(*)
        // After dedup by binder: SUM(qty), SUM(extprice), SUM(disc_price_expr),
        //     SUM(charge_expr), SUM(discount), COUNT(*)
        REQUIRE(agg->aggregates.size() >= 4);
    }

    // Child of HashAggregate should be PhysicalTableScan
    REQUIRE(agg->child != nullptr);
    REQUIRE(agg->child->kind == PhysicalNodeKind::TABLE_SCAN);
    auto* scan = static_cast<PhysicalTableScan*>(agg->child);

    SECTION("TableScan references lineitem table") {
        REQUIRE(scan->table->name == "lineitem");
    }

    SECTION("Predicate pushdown — filter merged into scan") {
        REQUIRE(scan->filter != nullptr);
        // The filter should be a comparison: l_shipdate <= DATE literal
        REQUIRE(scan->filter->kind == BoundExprKind::BINARY_EXPR);
        auto* filterExpr = static_cast<BoundBinaryExpr*>(scan->filter);
        REQUIRE(filterExpr->op == parser::BinaryOp::LE);
    }

    SECTION("Projection pushdown — scan reads only 7 of 16 columns") {
        // Q1 uses columns: l_quantity(4), l_extendedprice(5), l_discount(6),
        // l_tax(7), l_returnflag(8), l_linestatus(9), l_shipdate(10)
        REQUIRE(scan->columnIndices.size() == 7);

        std::set<uint32_t> expectedCols{4, 5, 6, 7, 8, 9, 10};
        std::set<uint32_t> actualCols(scan->columnIndices.begin(), scan->columnIndices.end());
        REQUIRE(actualCols == expectedCols);
    }
}

// ── Pipeline decomposition ──────────────────────────────────────────────────

TEST_CASE("Optimize TPC-H Q1 — pipeline decomposition", "[optimizer]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(Q1_SQL);
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    Optimizer opt;
    auto result = opt.optimize(bindResult);

    REQUIRE(result.pipelines.size() == 2);

    SECTION("Pipeline 0: build — TableScan to HashAggregate") {
        const auto& pipe0 = result.pipelines[0];
        REQUIRE(pipe0.source != nullptr);
        REQUIRE(pipe0.source->kind == PhysicalNodeKind::TABLE_SCAN);
        REQUIRE(pipe0.sink != nullptr);
        REQUIRE(pipe0.sink->kind == PhysicalNodeKind::HASH_AGGREGATE);
        REQUIRE(pipe0.description == "build");
    }

    SECTION("Pipeline 1: scan — HashAggregate scan side through to Sort") {
        const auto& pipe1 = result.pipelines[1];
        REQUIRE(pipe1.source != nullptr);
        REQUIRE(pipe1.source->kind == PhysicalNodeKind::HASH_AGGREGATE);
        REQUIRE(pipe1.sink != nullptr);
        REQUIRE(pipe1.sink->kind == PhysicalNodeKind::SORT);
        REQUIRE(pipe1.description == "scan");

        // Projection should be in the intermediate operators
        REQUIRE(pipe1.operators.size() == 1);
        REQUIRE(pipe1.operators[0]->kind == PhysicalNodeKind::PROJECTION);
    }
}

// ── Physical plan printer ───────────────────────────────────────────────────

TEST_CASE("Physical plan printer produces output", "[optimizer]") {
    auto cat = makeQ1Catalog();
    auto parseResult = Parser::parse(Q1_SQL);
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    Optimizer opt;
    auto result = opt.optimize(bindResult);

    SECTION("Plan printer") {
        std::string planStr = PhysicalPlanPrinter::printPlan(result.root);
        REQUIRE(!planStr.empty());
        // Should contain key operator names
        REQUIRE(planStr.find("PhysicalSort") != std::string::npos);
        REQUIRE(planStr.find("PhysicalProjection") != std::string::npos);
        REQUIRE(planStr.find("PhysicalHashAggregate") != std::string::npos);
        REQUIRE(planStr.find("PhysicalTableScan") != std::string::npos);
        REQUIRE(planStr.find("lineitem") != std::string::npos);
        // Should show column pruning
        REQUIRE(planStr.find("7/16 cols") != std::string::npos);
        // Should show filter was pushed down
        REQUIRE(planStr.find("filter:") != std::string::npos);
    }

    SECTION("Pipeline printer") {
        std::string pipeStr = PhysicalPlanPrinter::printPipelines(result.pipelines);
        REQUIRE(!pipeStr.empty());
        REQUIRE(pipeStr.find("Pipeline 0") != std::string::npos);
        REQUIRE(pipeStr.find("Pipeline 1") != std::string::npos);
        REQUIRE(pipeStr.find("TableScan") != std::string::npos);
        REQUIRE(pipeStr.find("HashAggregate") != std::string::npos);
    }
}

// ── Column pruning for required columns ─────────────────────────────────────

TEST_CASE("Projection pushdown collects correct columns", "[optimizer]") {
    // Create a simple catalog with a small table
    Catalog cat;
    TableDef tableDef;
    tableDef.name = "test_table";
    tableDef.columns = {
        {"a", SQLType::integer(), 0},
        {"b", SQLType::integer(), 1},
        {"c", SQLType::integer(), 2},
        {"d", SQLType::integer(), 3},
    };
    cat.addTable(std::move(tableDef));

    // Query that only uses columns a and c
    auto parseResult = Parser::parse("SELECT a, c FROM test_table WHERE a > 0;");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);
    REQUIRE(bindResult.root != nullptr);

    Optimizer opt;
    auto result = opt.optimize(bindResult);
    REQUIRE(result.root != nullptr);

    // Walk down to the TableScan
    // Root is Projection (no ORDER BY, no GROUP BY)
    REQUIRE(result.root->kind == PhysicalNodeKind::PROJECTION);
    auto* proj = static_cast<PhysicalProjection*>(result.root);
    REQUIRE(proj->child != nullptr);

    // Child should be TableScan with filter pushed down
    REQUIRE(proj->child->kind == PhysicalNodeKind::TABLE_SCAN);
    auto* scan = static_cast<PhysicalTableScan*>(proj->child);

    // Only columns 0 (a) and 2 (c) should be required
    std::set<uint32_t> actualCols(scan->columnIndices.begin(), scan->columnIndices.end());
    std::set<uint32_t> expectedCols{0, 2};
    REQUIRE(actualCols == expectedCols);

    // Filter should be pushed into scan
    REQUIRE(scan->filter != nullptr);
}

// ── Simple query without aggregation ────────────────────────────────────────

TEST_CASE("Optimize simple SELECT without aggregation", "[optimizer]") {
    Catalog cat;
    TableDef tableDef;
    tableDef.name = "t";
    tableDef.columns = {
        {"x", SQLType::integer(), 0},
        {"y", SQLType::integer(), 1},
    };
    cat.addTable(std::move(tableDef));

    auto parseResult = Parser::parse("SELECT x FROM t;");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    Optimizer opt;
    auto result = opt.optimize(bindResult);
    REQUIRE(result.root != nullptr);

    // Should be Projection -> TableScan
    REQUIRE(result.root->kind == PhysicalNodeKind::PROJECTION);
    auto* proj = static_cast<PhysicalProjection*>(result.root);
    REQUIRE(proj->child != nullptr);
    REQUIRE(proj->child->kind == PhysicalNodeKind::TABLE_SCAN);

    auto* scan = static_cast<PhysicalTableScan*>(proj->child);
    // Only column 0 (x) should be in the scan
    REQUIRE(scan->columnIndices.size() == 1);
    REQUIRE(scan->columnIndices[0] == 0);

    // No filter
    REQUIRE(scan->filter == nullptr);

    // Single pipeline (no pipeline breaks)
    REQUIRE(result.pipelines.size() == 1);
}

// ── Null root handling ──────────────────────────────────────────────────────

TEST_CASE("Optimizer handles null root", "[optimizer]") {
    BindResult empty;
    empty.root = nullptr;

    Optimizer opt;
    auto result = opt.optimize(empty);
    REQUIRE(result.root == nullptr);
    REQUIRE(result.pipelines.empty());
}

// ── Printer null handling ───────────────────────────────────────────────────

TEST_CASE("Physical plan printer handles null", "[optimizer]") {
    std::string planStr = PhysicalPlanPrinter::printPlan(nullptr);
    REQUIRE(planStr.find("(null)") != std::string::npos);

    std::vector<Pipeline> empty;
    std::string pipeStr = PhysicalPlanPrinter::printPipelines(empty);
    REQUIRE(pipeStr.empty());
}

// ── Single-pipeline with intermediate operators ─────────────────────────────

TEST_CASE("Single pipeline with intermediate operators (no break)", "[optimizer]") {
    Catalog cat;
    TableDef tableDef;
    tableDef.name = "t";
    tableDef.columns = {
        {"x", SQLType::integer(), 0},
        {"y", SQLType::integer(), 1},
    };
    cat.addTable(std::move(tableDef));

    // SELECT x FROM t ORDER BY x — produces Sort -> Projection -> TableScan
    // No HashAggregate, so no pipeline break — single pipeline.
    auto parseResult = Parser::parse("SELECT x FROM t ORDER BY x;");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    Optimizer opt;
    auto result = opt.optimize(bindResult);
    REQUIRE(result.root != nullptr);

    // Root should be Sort
    REQUIRE(result.root->kind == PhysicalNodeKind::SORT);

    // Single pipeline (no hash aggregate means no pipeline break)
    REQUIRE(result.pipelines.size() == 1);
    REQUIRE(result.pipelines[0].description == "single pipeline");
    REQUIRE(result.pipelines[0].source->kind == PhysicalNodeKind::TABLE_SCAN);
    REQUIRE(result.pipelines[0].sink->kind == PhysicalNodeKind::SORT);
    // Projection is in the middle
    REQUIRE(result.pipelines[0].operators.size() == 1);
    REQUIRE(result.pipelines[0].operators[0]->kind == PhysicalNodeKind::PROJECTION);
}

// ── Unary expression in column collection ───────────────────────────────────

TEST_CASE("Projection pushdown with unary expression", "[optimizer]") {
    Catalog cat;
    TableDef tableDef;
    tableDef.name = "t";
    tableDef.columns = {
        {"a", SQLType::integer(), 0},
        {"b", SQLType::integer(), 1},
        {"c", SQLType::integer(), 2},
    };
    cat.addTable(std::move(tableDef));

    // -a uses a unary negation on column a
    auto parseResult = Parser::parse("SELECT -a FROM t;");
    REQUIRE(parseResult.stmt != nullptr);

    Binder binder(cat);
    auto bindResult = binder.bind(*parseResult.stmt);

    Optimizer opt;
    auto result = opt.optimize(bindResult);
    REQUIRE(result.root != nullptr);

    // Walk down to scan and verify only column 0 is needed
    REQUIRE(result.root->kind == PhysicalNodeKind::PROJECTION);
    auto* proj = static_cast<PhysicalProjection*>(result.root);
    REQUIRE(proj->child != nullptr);
    REQUIRE(proj->child->kind == PhysicalNodeKind::TABLE_SCAN);
    auto* scan = static_cast<PhysicalTableScan*>(proj->child);

    std::set<uint32_t> actualCols(scan->columnIndices.begin(), scan->columnIndices.end());
    REQUIRE(actualCols == std::set<uint32_t>{0});
}

// ── Re-optimization resets state ────────────────────────────────────────────

TEST_CASE("Optimizer can be reused for multiple optimizations", "[optimizer]") {
    Optimizer opt;

    // First optimization
    auto cat1 = makeQ1Catalog();
    auto parseResult1 = Parser::parse(Q1_SQL);
    Binder binder1(cat1);
    auto bindResult1 = binder1.bind(*parseResult1.stmt);
    auto result1 = opt.optimize(bindResult1);
    REQUIRE(result1.root != nullptr);
    REQUIRE(result1.pipelines.size() == 2);

    // Second optimization (should reset state)
    auto cat2 = makeQ1Catalog();
    auto parseResult2 = Parser::parse(Q1_SQL);
    Binder binder2(cat2);
    auto bindResult2 = binder2.bind(*parseResult2.stmt);
    auto result2 = opt.optimize(bindResult2);
    REQUIRE(result2.root != nullptr);
    REQUIRE(result2.pipelines.size() == 2);
}
