#include <catch2/catch_test_macros.hpp>
#include <string>

#include "catalog/catalog.h"
#include "common/types.h"
#include "execution/pipeline.h"
#include "explain/explain.h"
#include "storage/column.h"
#include "storage/table.h"
#include "storage/tbl_loader.h"

using namespace umbra;
using namespace umbra::explain;

namespace {

/// Create a minimal test catalog with lineitem table.
Catalog makeTestCatalog() {
    Catalog catalog;
    catalog.addTable(lineitemTableDef());
    return catalog;
}

/// Create an empty lineitem table (no data loaded).
Table makeEmptyTable() {
    Table table;
    table.name = "lineitem";
    table.rowCount = 0;
    return table;
}

/// Q1-compatible catalog using BOOLEAN for char columns (matches test_q1.cpp).
Catalog makeQ1Catalog() {
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

/// Build a small test table with 10 rows (8 pass Q1 date filter).
Table makeQ1TestTable() {
    Table table;
    table.name = "lineitem";
    constexpr int N = 10;

    // Columns 0-3: placeholders (INTEGER).
    for (int c = 0; c < 4; ++c) {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage).resize(N, 0);
        table.columns.push_back(std::move(col));
    }
    // Column 4: l_quantity (DECIMAL scaled x100).
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {1000, 2000, 3000, 1500, 2500,
                                                       4000, 1200, 5000, 1800, 2200};
        table.columns.push_back(std::move(col));
    }
    // Column 5: l_extendedprice.
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {10000, 20000, 30000, 15000, 25000,
                                                       40000, 12000, 50000, 18000, 22000};
        table.columns.push_back(std::move(col));
    }
    // Column 6: l_discount.
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {5, 10, 0, 5, 8, 0, 2, 3, 7, 6};
        table.columns.push_back(std::move(col));
    }
    // Column 7: l_tax.
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {8, 4, 2, 6, 3, 0, 1, 5, 4, 2};
        table.columns.push_back(std::move(col));
    }
    // Column 8: l_returnflag (CHAR(1) — ASCII).
    {
        Column col = Column::create(SQLType::fixedChar(1));
        std::get<std::vector<int8_t>>(col.storage) = {65, 65, 78, 78, 82, 65, 78, 82, 65, 78};
        table.columns.push_back(std::move(col));
    }
    // Column 9: l_linestatus.
    {
        Column col = Column::create(SQLType::fixedChar(1));
        std::get<std::vector<int8_t>>(col.storage) = {70, 70, 79, 79, 70, 70, 79, 70, 70, 79};
        table.columns.push_back(std::move(col));
    }
    // Column 10: l_shipdate (days since 2000-01-01).
    {
        Column col = Column::create(SQLType::date());
        std::get<std::vector<int32_t>>(col.storage) = {-519, -505, -550, -580, -611,
                                                       -488, -488, -500, -365, -365};
        table.columns.push_back(std::move(col));
    }

    table.rowCount = N;
    table.finalize();
    return table;
}

/// TPC-H Q1 SQL.
const char* Q1_SQL =
    "SELECT l_returnflag, l_linestatus, "
    "SUM(l_quantity), SUM(l_extendedprice), "
    "SUM(l_extendedprice * (1 - l_discount)), "
    "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
    "AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), "
    "COUNT(*) "
    "FROM lineitem "
    "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
    "GROUP BY l_returnflag, l_linestatus "
    "ORDER BY l_returnflag, l_linestatus";

}  // namespace

TEST_CASE("ExplainLevel parsing", "[explain]") {
    ExplainLevel level{};

    SECTION("Valid levels") {
        REQUIRE(parseExplainLevel("AST", level));
        REQUIRE(level == ExplainLevel::AST);

        REQUIRE(parseExplainLevel("LOGICAL", level));
        REQUIRE(level == ExplainLevel::LOGICAL);

        REQUIRE(parseExplainLevel("PHYSICAL", level));
        REQUIRE(level == ExplainLevel::PHYSICAL);

        REQUIRE(parseExplainLevel("PIPELINE", level));
        REQUIRE(level == ExplainLevel::PIPELINE);

        REQUIRE(parseExplainLevel("IR", level));
        REQUIRE(level == ExplainLevel::IR);

        REQUIRE(parseExplainLevel("ASM", level));
        REQUIRE(level == ExplainLevel::ASM);

        REQUIRE(parseExplainLevel("PROFILE", level));
        REQUIRE(level == ExplainLevel::PROFILE);

        REQUIRE(parseExplainLevel("ALL", level));
        REQUIRE(level == ExplainLevel::ALL);
    }

    SECTION("Case insensitive") {
        REQUIRE(parseExplainLevel("ast", level));
        REQUIRE(level == ExplainLevel::AST);

        REQUIRE(parseExplainLevel("Logical", level));
        REQUIRE(level == ExplainLevel::LOGICAL);

        REQUIRE(parseExplainLevel("physical", level));
        REQUIRE(level == ExplainLevel::PHYSICAL);
    }

    SECTION("Invalid levels") {
        REQUIRE_FALSE(parseExplainLevel("INVALID", level));
        REQUIRE_FALSE(parseExplainLevel("", level));
        REQUIRE_FALSE(parseExplainLevel("SELECT", level));
    }
}

TEST_CASE("ExplainLevel to string", "[explain]") {
    REQUIRE(explainLevelToString(ExplainLevel::AST) == "AST");
    REQUIRE(explainLevelToString(ExplainLevel::LOGICAL) == "LOGICAL");
    REQUIRE(explainLevelToString(ExplainLevel::PHYSICAL) == "PHYSICAL");
    REQUIRE(explainLevelToString(ExplainLevel::PIPELINE) == "PIPELINE");
    REQUIRE(explainLevelToString(ExplainLevel::IR) == "IR");
    REQUIRE(explainLevelToString(ExplainLevel::ASM) == "ASM");
    REQUIRE(explainLevelToString(ExplainLevel::PROFILE) == "PROFILE");
    REQUIRE(explainLevelToString(ExplainLevel::ALL) == "ALL");
}

TEST_CASE("parseExplainPrefix", "[explain]") {
    ExplainLevel level{};
    std::string remainingSql;

    SECTION("EXPLAIN AST SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN AST SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::AST);
        REQUIRE(remainingSql == "SELECT 1");
    }

    SECTION("EXPLAIN LOGICAL SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN LOGICAL SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::LOGICAL);
        REQUIRE(remainingSql == "SELECT 1");
    }

    SECTION("EXPLAIN PHYSICAL SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN PHYSICAL SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::PHYSICAL);
    }

    SECTION("EXPLAIN PIPELINE SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN PIPELINE SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::PIPELINE);
    }

    SECTION("EXPLAIN IR SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN IR SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::IR);
    }

    SECTION("EXPLAIN ALL SELECT ...") {
        REQUIRE(parseExplainPrefix("EXPLAIN ALL SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::ALL);
    }

    SECTION("EXPLAIN without level defaults to ALL") {
        REQUIRE(parseExplainPrefix("EXPLAIN SELECT 1", level, remainingSql));
        REQUIRE(level == ExplainLevel::ALL);
        REQUIRE(remainingSql == "SELECT 1");
    }

    SECTION("Non-EXPLAIN returns false") {
        REQUIRE_FALSE(parseExplainPrefix("SELECT 1", level, remainingSql));
    }

    SECTION("Empty string returns false") {
        REQUIRE_FALSE(parseExplainPrefix("", level, remainingSql));
    }

    SECTION("Leading/trailing whitespace") {
        REQUIRE(parseExplainPrefix("  EXPLAIN AST  SELECT 1  ", level, remainingSql));
        REQUIRE(level == ExplainLevel::AST);
        REQUIRE(remainingSql == "SELECT 1");
    }
}

TEST_CASE("EXPLAIN AST produces output", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql = "SELECT l_returnflag FROM lineitem WHERE l_shipdate <= DATE '1998-09-02'";
    std::string result = runExplain(ExplainLevel::AST, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("AST:") != std::string::npos);
    REQUIRE(result.find("SelectStmt") != std::string::npos);
}

TEST_CASE("EXPLAIN LOGICAL produces output", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql =
        "SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-09-02' "
        "GROUP BY l_returnflag ORDER BY l_returnflag";
    std::string result = runExplain(ExplainLevel::LOGICAL, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("Logical Plan:") != std::string::npos);
}

TEST_CASE("EXPLAIN PHYSICAL produces output", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql =
        "SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-09-02' "
        "GROUP BY l_returnflag ORDER BY l_returnflag";
    std::string result = runExplain(ExplainLevel::PHYSICAL, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("Physical Plan:") != std::string::npos);
    REQUIRE(result.find("PhysicalTableScan") != std::string::npos);
}

TEST_CASE("EXPLAIN PIPELINE produces output", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql =
        "SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-09-02' "
        "GROUP BY l_returnflag ORDER BY l_returnflag";
    std::string result = runExplain(ExplainLevel::PIPELINE, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("Pipelines:") != std::string::npos);
    REQUIRE(result.find("Pipeline 0") != std::string::npos);
}

TEST_CASE("EXPLAIN IR produces output", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql =
        "SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-09-02' "
        "GROUP BY l_returnflag ORDER BY l_returnflag";
    std::string result = runExplain(ExplainLevel::IR, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("IR:") != std::string::npos);
}

TEST_CASE("EXPLAIN ALL produces output from multiple stages", "[explain]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    std::string sql =
        "SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
        "WHERE l_shipdate <= DATE '1998-09-02' "
        "GROUP BY l_returnflag ORDER BY l_returnflag";
    std::string result = runExplain(ExplainLevel::ALL, sql, catalog, table);

    REQUIRE_FALSE(result.empty());
    REQUIRE(result.find("AST:") != std::string::npos);
    REQUIRE(result.find("Logical Plan:") != std::string::npos);
    REQUIRE(result.find("Physical Plan:") != std::string::npos);
    REQUIRE(result.find("Pipelines:") != std::string::npos);
    REQUIRE(result.find("IR:") != std::string::npos);
}

// ── EXPLAIN PROFILE with TPC-H Q1 ──────────────────────────────────────────

TEST_CASE("EXPLAIN PROFILE Q1 — interpreter", "[explain][profile]") {
    auto catalog = makeQ1Catalog();
    auto table = makeQ1TestTable();

    std::string result =
        runExplain(ExplainLevel::PROFILE, Q1_SQL, catalog, table, execution::Backend::INTERPRETER);

    // Must contain per-pipeline breakdown header and rows.
    REQUIRE(result.find("Profile (backend: interpreter)") != std::string::npos);
    REQUIRE(result.find("plan") != std::string::npos);
    REQUIRE(result.find("codegen") != std::string::npos);
    REQUIRE(result.find("compile") != std::string::npos);
    REQUIRE(result.find("execute") != std::string::npos);
    REQUIRE(result.find("Pipeline 0") != std::string::npos);
    REQUIRE(result.find("Pipeline 1") != std::string::npos);
    REQUIRE(result.find("Total:") != std::string::npos);
    // Must show 3 result rows (A-F, N-O, R-F).
    REQUIRE(result.find("Result: 3 rows") != std::string::npos);
}

TEST_CASE("EXPLAIN PROFILE Q1 — flying-start", "[explain][profile]") {
    auto catalog = makeQ1Catalog();
    auto table = makeQ1TestTable();

    std::string result =
        runExplain(ExplainLevel::PROFILE, Q1_SQL, catalog, table, execution::Backend::FLYING_START);

    REQUIRE(result.find("Profile (backend: flying-start)") != std::string::npos);
    REQUIRE(result.find("Pipeline 0") != std::string::npos);
    REQUIRE(result.find("Pipeline 1") != std::string::npos);
    REQUIRE(result.find("Total:") != std::string::npos);
    REQUIRE(result.find("Result: 3 rows") != std::string::npos);
}

TEST_CASE("EXPLAIN PROFILE shows all explain stages before profile", "[explain][profile]") {
    auto catalog = makeQ1Catalog();
    auto table = makeQ1TestTable();

    std::string result = runExplain(ExplainLevel::PROFILE, Q1_SQL, catalog, table);

    // PROFILE shows AST, Logical, Physical, Pipeline, IR sections too.
    REQUIRE(result.find("AST:") != std::string::npos);
    REQUIRE(result.find("Logical Plan:") != std::string::npos);
    REQUIRE(result.find("Physical Plan:") != std::string::npos);
    REQUIRE(result.find("Pipelines:") != std::string::npos);
    REQUIRE(result.find("IR:") != std::string::npos);
    REQUIRE(result.find("Profile") != std::string::npos);
}
