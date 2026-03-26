#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/types.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "storage/column.h"
#include "storage/table.h"

using namespace umbra;
using namespace umbra::execution;

// ── Shared helpers (same as test_q1.cpp) ────────────────────────────────────

static Catalog makeQ1Catalog() {
    Catalog catalog;
    TableDef def;
    def.name = "lineitem";
    def.columns = {
        {"l_orderkey", SQLType::integer(), 0},      {"l_partkey", SQLType::integer(), 1},
        {"l_suppkey", SQLType::integer(), 2},       {"l_linenumber", SQLType::integer(), 3},
        {"l_quantity", SQLType::decimal(15, 2), 4}, {"l_extendedprice", SQLType::decimal(15, 2), 5},
        {"l_discount", SQLType::decimal(15, 2), 6}, {"l_tax", SQLType::decimal(15, 2), 7},
        {"l_returnflag", SQLType::fixedChar(1), 8}, {"l_linestatus", SQLType::fixedChar(1), 9},
        {"l_shipdate", SQLType::date(), 10},
    };
    catalog.addTable(std::move(def));
    return catalog;
}

// ── Test table with dates spread across a wide range ─────────────────────────
// This allows testing different INTERVAL values.
//
// Rows with progressively later ship dates:
//   Row 0:  1998-06-01 (day -580)  — passes all cutoffs
//   Row 1:  1998-07-01 (day -550)  — passes 60/90/120
//   Row 2:  1998-08-01 (day -519)  — passes 60/90/120
//   Row 3:  1998-09-01 (day -488)  — passes 90/120, not 60
//   Row 4:  1998-09-15 (day -474)  — passes 120, not 60/90
//   Row 5:  1998-10-01 (day -458)  — passes 120, not 60/90
//   Row 6:  1998-10-15 (day -444)  — fails all cutoffs
//   Row 7:  1998-11-01 (day -427)  — fails all cutoffs

static Table makeVariantsTable() {
    Table table;
    table.name = "lineitem";
    constexpr int N = 8;

    for (int c = 0; c < 4; ++c) {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage).resize(N, 0);
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {10000, 10000, 10000, 10000, 10000, 10000, 10000, 10000};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {5, 5, 5, 5, 5, 5, 5, 5};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {8, 8, 8, 8, 8, 8, 8, 8};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v = {65, 65, 65, 65, 65, 65, 65, 65};  // All 'A'
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v = {70, 70, 70, 70, 70, 70, 70, 70};  // All 'F'
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::date());
        auto& v = std::get<std::vector<int32_t>>(col.storage);
        v = {-580, -550, -519, -488, -474, -458, -444, -427};
        table.columns.push_back(std::move(col));
    }

    table.rowCount = N;
    table.finalize();
    return table;
}

static ResultCollector executeSQL(Catalog& catalog, Table& table, const std::string& sql) {
    auto parseResult = parser::Parser::parse(sql);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);
    ResultCollector collector;
    PipelineExecutor executor;
    executor.execute(optResult, table, collector);
    return collector;
}

// ── SQL templates for different INTERVAL values ──────────────────────────────

static std::string makeQ1WithInterval(int days) {
    return "SELECT l_returnflag, l_linestatus, "
           "SUM(l_quantity), SUM(l_extendedprice), "
           "SUM(l_extendedprice * (1 - l_discount)), "
           "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
           "AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), "
           "COUNT(*) "
           "FROM lineitem "
           "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '" +
           std::to_string(days) +
           "' DAY "
           "GROUP BY l_returnflag, l_linestatus "
           "ORDER BY l_returnflag, l_linestatus";
}

// ── Tests: INTERVAL variants ─────────────────────────────────────────────────

TEST_CASE("Q1 INTERVAL 60 DAY", "[sql][q1_variants]") {
    // Cutoff: 1998-12-01 - 60 = 1998-10-02 (day ~-457)
    // Rows with shipdate <= -457: rows 0,1,2,3,4,5 (6 rows pass)
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(60));

    // All rows in a single group (A-F), so 1 output row.
    REQUIRE(collector.numRows() == 1);

    // 6 rows pass the filter.
    CHECK(collector.rows()[0].values[9] == 6);  // COUNT(*)
}

TEST_CASE("Q1 INTERVAL 90 DAY (standard)", "[sql][q1_variants]") {
    // Cutoff: 1998-12-01 - 90 = 1998-09-02 (day ~-487)
    // Rows with shipdate <= -487: rows 0,1,2 (3 rows), row 3 is exactly -488 (passes).
    // Actually -488 <= -487? No: -488 < -487 so it passes LE.
    // Rows: 0(-580), 1(-550), 2(-519), 3(-488) => 4 rows pass.
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(90));

    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[9] == 4);  // COUNT(*)
}

TEST_CASE("Q1 INTERVAL 120 DAY", "[sql][q1_variants]") {
    // Cutoff: 1998-12-01 - 120 = 1998-08-03 (day ~-517)
    // Rows with shipdate <= -517: rows 0(-580), 1(-550), 2(-519) => 3 rows pass.
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(120));

    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[9] == 3);  // COUNT(*)
}

// ── Edge case: very permissive filter (all rows pass) ────────────────────────

TEST_CASE("Q1 variant: all rows pass filter", "[sql][q1_variants]") {
    // Use INTERVAL '1' DAY (cutoff 1998-11-30) — all 8 rows pass.
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(1));

    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[9] == 8);  // COUNT(*)
}

// ── Edge case: very restrictive filter (no rows pass) ────────────────────────

TEST_CASE("Q1 variant: very restrictive filter", "[sql][q1_variants]") {
    // Use INTERVAL '180' DAY (cutoff 1998-06-04 = day ~-576)
    // Only row 0 (-580) passes.
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(180));

    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[9] == 1);  // COUNT(*)
}

// ── Test: multiple groups with different intervals ───────────────────────────

TEST_CASE("Q1 variant: multi-group test", "[sql][q1_variants]") {
    // Create a table with multiple returnflag/linestatus groups and varying dates.
    auto catalog = makeQ1Catalog();

    Table table;
    table.name = "lineitem";
    constexpr int N = 6;

    for (int c = 0; c < 4; ++c) {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage).resize(N, 0);
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {1000, 2000, 3000, 4000, 5000, 6000};
        table.columns.push_back(std::move(col));
    }
    for (int c = 0; c < 3; ++c) {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage).resize(N, 0);
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::fixedChar(1));
        // 3 different flags: A, A, N, N, R, R
        std::get<std::vector<int8_t>>(col.storage) = {65, 65, 78, 78, 82, 82};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::fixedChar(1));
        std::get<std::vector<int8_t>>(col.storage) = {70, 70, 79, 79, 70, 70};
        table.columns.push_back(std::move(col));
    }
    {
        Column col = Column::create(SQLType::date());
        // All pass the standard Q1 filter.
        std::get<std::vector<int32_t>>(col.storage) = {-600, -600, -600, -600, -600, -600};
        table.columns.push_back(std::move(col));
    }

    table.rowCount = N;
    table.finalize();

    auto collector = executeSQL(catalog, table, makeQ1WithInterval(90));

    // 3 groups: A-F, N-O, R-F
    REQUIRE(collector.numRows() == 3);

    // Groups should be sorted by returnflag then linestatus.
    CHECK(collector.rows()[0].values[0] == 65);  // A
    CHECK(collector.rows()[1].values[0] == 78);  // N
    CHECK(collector.rows()[2].values[0] == 82);  // R

    // Each group has 2 rows, so sum_qty should be correct.
    CHECK(collector.rows()[0].values[2] == 3000);   // A-F: 1000 + 2000
    CHECK(collector.rows()[1].values[2] == 7000);   // N-O: 3000 + 4000
    CHECK(collector.rows()[2].values[2] == 11000);  // R-F: 5000 + 6000
}

// ── Test: consistent results across multiple executions ──────────────────────

TEST_CASE("Q1 variant: deterministic results", "[sql][q1_variants]") {
    auto catalog = makeQ1Catalog();
    auto table = makeVariantsTable();

    auto sql = makeQ1WithInterval(90);
    auto collector1 = executeSQL(catalog, table, sql);
    auto collector2 = executeSQL(catalog, table, sql);

    REQUIRE(collector1.numRows() == collector2.numRows());
    for (size_t r = 0; r < collector1.rows().size(); ++r) {
        for (size_t c = 0; c < collector1.rows()[r].values.size(); ++c) {
            CHECK(collector1.rows()[r].values[c] == collector2.rows()[r].values[c]);
        }
    }
}
