#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <string>

#include "catalog/catalog.h"
#include "shell/shell.h"
#include "storage/table.h"
#include "storage/tbl_loader.h"

using namespace umbra;
using namespace umbra::shell;

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

TEST_CASE("Shell dot commands", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    SECTION(".help") {
        REQUIRE(shell.executeStatement(".help", out, err));
        std::string result = out.str();
        REQUIRE(result.find(".help") != std::string::npos);
        REQUIRE(result.find(".quit") != std::string::npos);
        REQUIRE(result.find(".tables") != std::string::npos);
        REQUIRE(result.find(".schema") != std::string::npos);
        REQUIRE(result.find(".timer") != std::string::npos);
    }

    SECTION(".tables") {
        REQUIRE(shell.executeStatement(".tables", out, err));
        std::string result = out.str();
        REQUIRE(result.find("lineitem") != std::string::npos);
    }

    SECTION(".schema lineitem") {
        REQUIRE(shell.executeStatement(".schema lineitem", out, err));
        std::string result = out.str();
        REQUIRE(result.find("Table: lineitem") != std::string::npos);
        REQUIRE(result.find("l_orderkey") != std::string::npos);
        REQUIRE(result.find("l_quantity") != std::string::npos);
    }

    SECTION(".schema without argument lists all") {
        REQUIRE(shell.executeStatement(".schema", out, err));
        std::string result = out.str();
        REQUIRE(result.find("lineitem") != std::string::npos);
    }

    SECTION(".schema nonexistent table") {
        REQUIRE(shell.executeStatement(".schema nonexistent", out, err));
        std::string errResult = err.str();
        REQUIRE(errResult.find("not found") != std::string::npos);
    }

    SECTION(".timer on") {
        REQUIRE(shell.executeStatement(".timer on", out, err));
        REQUIRE(out.str().find("Timer enabled") != std::string::npos);
    }

    SECTION(".timer off") {
        REQUIRE(shell.executeStatement(".timer off", out, err));
        REQUIRE(out.str().find("Timer disabled") != std::string::npos);
    }

    SECTION(".timer bad arg") {
        REQUIRE(shell.executeStatement(".timer bad", out, err));
        REQUIRE(err.str().find("Usage") != std::string::npos);
    }

    SECTION(".quit returns false") {
        REQUIRE_FALSE(shell.executeStatement(".quit", out, err));
    }

    SECTION(".exit returns false") {
        REQUIRE_FALSE(shell.executeStatement(".exit", out, err));
    }

    SECTION("unknown command") {
        REQUIRE(shell.executeStatement(".unknown", out, err));
        REQUIRE(err.str().find("unknown command") != std::string::npos);
    }
}

TEST_CASE("Shell executeStatement - empty/whitespace", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    SECTION("Empty string") {
        REQUIRE(shell.executeStatement("", out, err));
        REQUIRE(out.str().empty());
    }

    SECTION("Whitespace only") {
        REQUIRE(shell.executeStatement("   ", out, err));
        REQUIRE(out.str().empty());
    }

    SECTION("Semicolon only") {
        REQUIRE(shell.executeStatement(";", out, err));
        REQUIRE(out.str().empty());
    }
}

TEST_CASE("Shell EXPLAIN via executeStatement", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    SECTION("EXPLAIN AST") {
        REQUIRE(shell.executeStatement(
            "EXPLAIN AST SELECT l_returnflag FROM lineitem WHERE l_shipdate <= DATE '1998-09-02'",
            out, err));
        REQUIRE(out.str().find("AST:") != std::string::npos);
    }

    SECTION("EXPLAIN LOGICAL") {
        REQUIRE(shell.executeStatement(
            "EXPLAIN LOGICAL SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
            "WHERE l_shipdate <= DATE '1998-09-02' "
            "GROUP BY l_returnflag ORDER BY l_returnflag",
            out, err));
        REQUIRE(out.str().find("Logical Plan:") != std::string::npos);
    }

    SECTION("EXPLAIN PHYSICAL") {
        REQUIRE(shell.executeStatement(
            "EXPLAIN PHYSICAL SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
            "WHERE l_shipdate <= DATE '1998-09-02' "
            "GROUP BY l_returnflag ORDER BY l_returnflag",
            out, err));
        REQUIRE(out.str().find("Physical Plan:") != std::string::npos);
    }

    SECTION("EXPLAIN PIPELINE") {
        REQUIRE(shell.executeStatement(
            "EXPLAIN PIPELINE SELECT l_returnflag, SUM(l_quantity) FROM lineitem "
            "WHERE l_shipdate <= DATE '1998-09-02' "
            "GROUP BY l_returnflag ORDER BY l_returnflag",
            out, err));
        REQUIRE(out.str().find("Pipelines:") != std::string::npos);
    }
}

TEST_CASE("Shell SQL error handling", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    SECTION("Syntax error") {
        shell.executeStatement("SELEC * FROM lineitem", out, err);
        REQUIRE(err.str().find("error") != std::string::npos);
    }

    SECTION("Bind error - unknown column") {
        shell.executeStatement(
            "SELECT l_unknown FROM lineitem "
            "WHERE l_shipdate <= DATE '1998-09-02' "
            "GROUP BY l_unknown ORDER BY l_unknown",
            out, err);
        REQUIRE(err.str().find("error") != std::string::npos);
    }
}

TEST_CASE("Shell printBanner", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = true;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    shell.printBanner(out);
    std::string result = out.str();
    REQUIRE(result.find("UmbraDB") != std::string::npos);
    REQUIRE(result.find("rows loaded") != std::string::npos);
    REQUIRE(result.find(".help") != std::string::npos);
}

TEST_CASE("Shell banner", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = true;
    Shell shell(catalog, table, config);

    // We can't easily test the full run() method since it reads from stdin,
    // but we can verify the shell is constructed properly.
    // The banner is printed during run() when interactive.
    // For unit testing, we test the public executeStatement API instead.

    std::ostringstream out;
    std::ostringstream err;
    REQUIRE(shell.executeStatement(".help", out, err));
    REQUIRE_FALSE(out.str().empty());
}

TEST_CASE("Shell readStatement", "[shell]") {
    SECTION("Single line with semicolon") {
        std::istringstream in("SELECT 1;\n");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt == "SELECT 1;");
    }

    SECTION("Multi-line input") {
        std::istringstream in("SELECT\n1;\n");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt.find("SELECT") != std::string::npos);
        REQUIRE(stmt.find("1;") != std::string::npos);
    }

    SECTION("Dot command (no semicolon needed)") {
        std::istringstream in(".help\n");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt == ".help");
    }

    SECTION("EOF returns empty") {
        std::istringstream in("");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt.empty());
    }

    SECTION("EOF with partial buffer") {
        std::istringstream in("SELECT 1");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt == "SELECT 1");
    }

    SECTION("Interactive mode still reads correctly") {
        std::istringstream in("SELECT 1;\n");
        // Note: this will print prompts to cout.
        std::string stmt = Shell::readStatement(in, true);
        REQUIRE(stmt.find("SELECT 1;") != std::string::npos);
    }

    SECTION("Interactive multi-line") {
        std::istringstream in("SELECT\n1;\n");
        std::string stmt = Shell::readStatement(in, true);
        REQUIRE(stmt.find("1;") != std::string::npos);
    }

    SECTION("Multiple lines before semicolon") {
        std::istringstream in("SELECT\nl_returnflag\nFROM lineitem;\n");
        std::string stmt = Shell::readStatement(in, false);
        REQUIRE(stmt.find("SELECT") != std::string::npos);
        REQUIRE(stmt.find("l_returnflag") != std::string::npos);
        REQUIRE(stmt.find("FROM lineitem;") != std::string::npos);
    }
}

TEST_CASE("Shell executeStatement with semicolon", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    SECTION("Statement with semicolon") {
        REQUIRE(shell.executeStatement(".help;", out, err));
        REQUIRE(out.str().find(".help") != std::string::npos);
    }

    SECTION("Only semicolon with spaces") {
        REQUIRE(shell.executeStatement("  ;  ", out, err));
        REQUIRE(out.str().empty());
    }
}

TEST_CASE("Shell parse error in executeQuery", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    // Invalid SQL should trigger a parse error.
    shell.executeStatement("INVALID SQL STATEMENT HERE", out, err);
    REQUIRE_FALSE(err.str().empty());
}

TEST_CASE("Shell .schema shows all type names", "[shell]") {
    // Create a catalog with custom table that has all types.
    Catalog catalog;
    TableDef def;
    def.name = "alltypes";
    def.columns = {
        {"col_int", SQLType::integer(), 0},       {"col_bigint", SQLType::bigint(), 1},
        {"col_date", SQLType::date(), 2},         {"col_decimal", SQLType::decimal(15, 2), 3},
        {"col_varchar", SQLType::varchar(50), 4}, {"col_bool", SQLType::boolean(), 5},
    };
    catalog.addTable(def);

    auto table = makeEmptyTable();
    ShellConfig config;
    config.interactive = false;
    Shell shell(catalog, table, config);

    std::ostringstream out;
    std::ostringstream err;

    shell.executeStatement(".schema alltypes", out, err);
    std::string result = out.str();

    REQUIRE(result.find("INTEGER") != std::string::npos);
    REQUIRE(result.find("BIGINT") != std::string::npos);
    REQUIRE(result.find("DATE") != std::string::npos);
    REQUIRE(result.find("DECIMAL(15,2)") != std::string::npos);
    REQUIRE(result.find("VARCHAR(50)") != std::string::npos);
    REQUIRE(result.find("BOOLEAN") != std::string::npos);
}

TEST_CASE("Shell interactive formatting flag", "[shell]") {
    auto catalog = makeTestCatalog();
    auto table = makeEmptyTable();

    SECTION("Non-interactive mode") {
        ShellConfig config;
        config.interactive = false;
        Shell shell(catalog, table, config);

        std::ostringstream out;
        std::ostringstream err;

        // EXPLAIN should still work.
        shell.executeStatement(
            "EXPLAIN AST SELECT l_returnflag FROM lineitem WHERE l_shipdate <= DATE '1998-09-02'",
            out, err);
        REQUIRE(out.str().find("AST:") != std::string::npos);
    }

    SECTION("Interactive mode") {
        ShellConfig config;
        config.interactive = true;
        Shell shell(catalog, table, config);

        std::ostringstream out;
        std::ostringstream err;

        // Should still work with interactive flag.
        shell.executeStatement(".tables", out, err);
        REQUIRE(out.str().find("lineitem") != std::string::npos);
    }
}
