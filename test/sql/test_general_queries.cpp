#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
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

// ── Shared helpers ──────────────────────────────────────────────────────────

static Catalog makeCatalog() {
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

static Table makeTable() {
    Table table;
    table.name = "lineitem";
    constexpr int N = 6;

    // l_orderkey (col 0): 1, 1, 2, 2, 3, 3
    {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage) = {1, 1, 2, 2, 3, 3};
        table.columns.push_back(std::move(col));
    }
    // l_partkey (col 1)
    {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage) = {10, 20, 30, 40, 50, 60};
        table.columns.push_back(std::move(col));
    }
    // l_suppkey (col 2)
    {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage) = {100, 200, 300, 400, 500, 600};
        table.columns.push_back(std::move(col));
    }
    // l_linenumber (col 3)
    {
        Column col = Column::create(SQLType::integer());
        std::get<std::vector<int32_t>>(col.storage) = {1, 2, 1, 2, 1, 2};
        table.columns.push_back(std::move(col));
    }
    // l_quantity (col 4): decimal(15,2), stored as int64 * 100
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {1000, 2000, 3000, 4000, 5000, 6000};  // 10.00, 20.00, ..., 60.00
        table.columns.push_back(std::move(col));
    }
    // l_extendedprice (col 5): decimal(15,2)
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {10000, 20000, 30000, 40000, 50000, 60000};
        table.columns.push_back(std::move(col));
    }
    // l_discount (col 6): decimal(15,2)
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {5, 5, 10, 10, 5, 5};  // 0.05, 0.05, 0.10, 0.10, 0.05, 0.05
        table.columns.push_back(std::move(col));
    }
    // l_tax (col 7): decimal(15,2)
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v = {8, 8, 8, 8, 8, 8};  // 0.08
        table.columns.push_back(std::move(col));
    }
    // l_returnflag (col 8): CHAR(1)
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v = {65, 65, 65, 78, 78, 82};  // A, A, A, N, N, R
        table.columns.push_back(std::move(col));
    }
    // l_linestatus (col 9): CHAR(1)
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v = {70, 70, 70, 79, 79, 70};  // F, F, F, O, O, F
        table.columns.push_back(std::move(col));
    }
    // l_shipdate (col 10): DATE (days since 2000-01-01)
    {
        Column col = Column::create(SQLType::date());
        auto& v = std::get<std::vector<int32_t>>(col.storage);
        v = {-580, -550, -519, -488, -474, -458};
        table.columns.push_back(std::move(col));
    }

    table.rowCount = N;
    table.finalize();
    return table;
}

static ResultCollector executeSQL(Catalog& catalog, Table& table, const std::string& sql,
                                  Backend backend) {
    auto parseResult = parser::Parser::parse(sql);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);
    ResultCollector collector;
    PipelineExecutor executor(backend);
    executor.execute(optResult, table, collector);
    return collector;
}

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
static const auto ALL_BACKENDS = std::vector<Backend>{
    Backend::INTERPRETER,
    Backend::FLYING_START,
#ifdef UMBRA_LLVM_BACKEND
    Backend::LLVM,
    Backend::ADAPTIVE,
#endif
};
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

// ── Test: single-pipeline scan (no aggregate) ──────────────────────────────

TEST_CASE("Single pipeline: SELECT with WHERE (integer)", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector =
        executeSQL(catalog, table, "SELECT l_orderkey FROM lineitem WHERE l_orderkey > 1", backend);
    REQUIRE(collector.numRows() == 4);
}

TEST_CASE("Single pipeline: SELECT with WHERE (decimal)", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_quantity, l_extendedprice FROM lineitem "
                                "WHERE l_quantity > 30.00",
                                backend);
    REQUIRE(collector.numRows() == 3);
    for (size_t i = 0; i < collector.numRows(); ++i) {
        CHECK(collector.rows()[i].values[0] > 3000);
    }
}

TEST_CASE("Single pipeline: SELECT all rows", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector =
        executeSQL(catalog, table, "SELECT l_orderkey, l_quantity FROM lineitem", backend);
    REQUIRE(collector.numRows() == 6);
}

TEST_CASE("Single pipeline: SELECT with ORDER BY", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_quantity, l_extendedprice FROM lineitem "
                                "ORDER BY l_quantity DESC",
                                backend);
    REQUIRE(collector.numRows() == 6);
    CHECK(collector.rows()[0].values[0] == 6000);
    CHECK(collector.rows()[5].values[0] == 1000);
}

TEST_CASE("Single pipeline: SELECT with WHERE and ORDER BY", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_quantity FROM lineitem "
                                "WHERE l_quantity > 20.00 "
                                "ORDER BY l_quantity",
                                backend);
    REQUIRE(collector.numRows() == 4);
    CHECK(collector.rows()[0].values[0] == 3000);
    CHECK(collector.rows()[3].values[0] == 6000);
}

// ── Test: aggregate with varying group key counts ───────────────────────────

TEST_CASE("Aggregate: single group key", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_returnflag, SUM(l_quantity), COUNT(*) "
                                "FROM lineitem "
                                "GROUP BY l_returnflag "
                                "ORDER BY l_returnflag",
                                backend);
    REQUIRE(collector.numRows() == 3);
    CHECK(collector.rows()[0].values[0] == 65);
    CHECK(collector.rows()[0].values[1] == 6000);
    CHECK(collector.rows()[0].values[2] == 3);
    CHECK(collector.rows()[1].values[0] == 78);
    CHECK(collector.rows()[1].values[1] == 9000);
    CHECK(collector.rows()[1].values[2] == 2);
    CHECK(collector.rows()[2].values[0] == 82);
    CHECK(collector.rows()[2].values[1] == 6000);
    CHECK(collector.rows()[2].values[2] == 1);
}

TEST_CASE("Aggregate: global aggregate (no GROUP BY)", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector =
        executeSQL(catalog, table, "SELECT SUM(l_quantity), COUNT(*) FROM lineitem", backend);
    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[0] == 21000);
    CHECK(collector.rows()[0].values[1] == 6);
}

TEST_CASE("Aggregate: GROUP BY integer column", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_orderkey, SUM(l_quantity), COUNT(*) "
                                "FROM lineitem "
                                "GROUP BY l_orderkey "
                                "ORDER BY l_orderkey",
                                backend);
    REQUIRE(collector.numRows() == 3);
    CHECK(collector.rows()[0].values[0] == 1);
    CHECK(collector.rows()[0].values[1] == 3000);
    CHECK(collector.rows()[0].values[2] == 2);
    CHECK(collector.rows()[1].values[0] == 2);
    CHECK(collector.rows()[1].values[1] == 7000);
    CHECK(collector.rows()[2].values[0] == 3);
    CHECK(collector.rows()[2].values[1] == 11000);
}

// ── Test: DECIMAL literal precision ─────────────────────────────────────────

TEST_CASE("DECIMAL literal with scale=3", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(
        catalog, table, "SELECT l_discount FROM lineitem WHERE l_discount > 0.005", backend);
    CHECK(collector.numRows() == 6);
}

// ── Test: expression in projection ──────────────────────────────────────────

TEST_CASE("Single pipeline: computed expression", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector = executeSQL(catalog, table,
                                "SELECT l_extendedprice * (1 - l_discount) FROM lineitem "
                                "WHERE l_orderkey = 1",
                                backend);
    REQUIRE(collector.numRows() == 2);
}

// ── Test: explicit CAST ─────────────────────────────────────────────────────

TEST_CASE("Explicit CAST: integer to bigint", "[sql][general]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    auto collector =
        executeSQL(catalog, table, "SELECT CAST(l_orderkey AS bigint) FROM lineitem", backend);
    REQUIRE(collector.numRows() == 6);
    CHECK(collector.rows()[0].values[0] == 1);
    CHECK(collector.rows()[5].values[0] == 3);
}
