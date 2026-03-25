#include <catch2/catch_test_macros.hpp>
#include <string>

#include "catalog/catalog.h"
#include "explain/explain.h"
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
