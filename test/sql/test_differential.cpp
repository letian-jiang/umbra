#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/types.h"
#include "duckdb.hpp"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "shell/formatter.h"
#include "storage/column.h"
#include "storage/table.h"

using namespace umbra;
using namespace umbra::execution;

// ── DuckDB C API helper ─────────────────────────────────────────────────────

/// RAII wrapper for a DuckDB in-memory database with test data pre-loaded.
class DuckDBOracle {
   public:
    DuckDBOracle() {
        duckdb_open(nullptr, &db_);
        duckdb_connect(db_, &conn_);
        exec(R"(
            CREATE TABLE lineitem (
                l_orderkey INTEGER,
                l_partkey INTEGER,
                l_suppkey INTEGER,
                l_linenumber INTEGER,
                l_quantity DECIMAL(15,2),
                l_extendedprice DECIMAL(15,2),
                l_discount DECIMAL(15,2),
                l_tax DECIMAL(15,2),
                l_returnflag VARCHAR,
                l_linestatus VARCHAR,
                l_shipdate DATE
            );
            INSERT INTO lineitem VALUES
                (1, 10, 100, 1, 10.00, 100.00, 0.05, 0.08, 'A', 'F', '1998-06-01'),
                (1, 20, 200, 2, 20.00, 200.00, 0.05, 0.08, 'A', 'F', '1998-07-01'),
                (2, 30, 300, 1, 30.00, 300.00, 0.10, 0.08, 'A', 'F', '1998-08-01'),
                (2, 40, 400, 2, 40.00, 400.00, 0.10, 0.08, 'N', 'O', '1998-09-01'),
                (3, 50, 500, 1, 50.00, 500.00, 0.05, 0.08, 'N', 'O', '1998-09-15'),
                (3, 60, 600, 2, 60.00, 600.00, 0.05, 0.08, 'R', 'F', '1998-10-01');
        )");
    }

    ~DuckDBOracle() {
        duckdb_disconnect(&conn_);
        duckdb_close(&db_);
    }

    DuckDBOracle(const DuckDBOracle&) = delete;
    DuckDBOracle& operator=(const DuckDBOracle&) = delete;
    DuckDBOracle(DuckDBOracle&&) = delete;
    DuckDBOracle& operator=(DuckDBOracle&&) = delete;

    /// Run a query and return rows as string cells.
    std::vector<std::vector<std::string>> query(const std::string& sql) {
        duckdb_result result;
        duckdb_state state = duckdb_query(conn_, sql.c_str(), &result);
        std::vector<std::vector<std::string>> rows;
        if (state == DuckDBError) {
            const char* err = duckdb_result_error(&result);
            rows.push_back({std::string("ERROR: ") + (err != nullptr ? err : "unknown")});
            duckdb_destroy_result(&result);
            return rows;
        }

        idx_t numRows = duckdb_row_count(&result);
        idx_t numCols = duckdb_column_count(&result);
        for (idx_t r = 0; r < numRows; ++r) {
            std::vector<std::string> row;
            for (idx_t c = 0; c < numCols; ++c) {
                char* val = duckdb_value_varchar(&result, c, r);
                row.emplace_back(val != nullptr ? val : "NULL");
                duckdb_free(val);
            }
            rows.push_back(std::move(row));
        }
        duckdb_destroy_result(&result);
        return rows;
    }

   private:
    duckdb_database db_{};
    duckdb_connection conn_{};

    void exec(const char* sql) {
        duckdb_result result;
        duckdb_query(conn_, sql, &result);
        duckdb_destroy_result(&result);
    }
};

// ── Umbra helpers ───────────────────────────────────────────────────────────

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

    // l_orderkey (col 0)
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
    // l_quantity (col 4): 10.00 .. 60.00
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {1000, 2000, 3000, 4000, 5000, 6000};
        table.columns.push_back(std::move(col));
    }
    // l_extendedprice (col 5): 100.00 .. 600.00
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {10000, 20000, 30000, 40000, 50000, 60000};
        table.columns.push_back(std::move(col));
    }
    // l_discount (col 6): 0.05, 0.05, 0.10, 0.10, 0.05, 0.05
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {5, 5, 10, 10, 5, 5};
        table.columns.push_back(std::move(col));
    }
    // l_tax (col 7): 0.08
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        std::get<std::vector<int64_t>>(col.storage) = {8, 8, 8, 8, 8, 8};
        table.columns.push_back(std::move(col));
    }
    // l_returnflag (col 8): A, A, A, N, N, R
    {
        Column col = Column::create(SQLType::fixedChar(1));
        std::get<std::vector<int8_t>>(col.storage) = {65, 65, 65, 78, 78, 82};
        table.columns.push_back(std::move(col));
    }
    // l_linestatus (col 9): F, F, F, O, O, F
    {
        Column col = Column::create(SQLType::fixedChar(1));
        std::get<std::vector<int8_t>>(col.storage) = {70, 70, 70, 79, 79, 70};
        table.columns.push_back(std::move(col));
    }
    // l_shipdate (col 10): 1998-06-01 .. 1998-10-01
    {
        Column col = Column::create(SQLType::date());
        std::get<std::vector<int32_t>>(col.storage) = {-580, -550, -519, -488, -474, -458};
        table.columns.push_back(std::move(col));
    }

    table.rowCount = N;
    table.finalize();
    return table;
}

/// Human-readable backend name for diagnostics.
static const char* backendName(Backend b) {
    switch (b) {
        case Backend::INTERPRETER:
            return "INTERPRETER";
        case Backend::FLYING_START:
            return "FLYING_START";
        case Backend::LLVM:
            return "LLVM";
        case Backend::ADAPTIVE:
            return "ADAPTIVE";
    }
    return "UNKNOWN";
}

/// Execute SQL against Umbra and return rows as string cells.
static std::vector<std::vector<std::string>> executeUmbra(Catalog& catalog, Table& table,
                                                          const std::string& sql, Backend backend) {
    auto parseResult = parser::Parser::parse(sql);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);
    ResultCollector collector;
    PipelineExecutor executor(backend);
    executor.execute(optResult, table, collector);

    const auto& schema = collector.schema();
    std::vector<std::vector<std::string>> rows;
    for (const auto& row : collector.rows()) {
        std::vector<std::string> cells;
        for (size_t c = 0; c < schema.size(); ++c) {
            cells.push_back(shell::formatValue(row.values[c], schema[c].type));
        }
        rows.push_back(std::move(cells));
    }
    return rows;
}

/// Compare two cell values approximately (for numeric values with rounding differences).
static bool approxEqual(const std::string& a, const std::string& b) {
    if (a == b) {
        return true;
    }
    try {
        double va = std::stod(a);
        double vb = std::stod(b);
        double denom = std::max({std::abs(va), std::abs(vb), 1.0});
        return std::abs(va - vb) / denom < 0.02;  // 2% relative tolerance
    } catch (const std::invalid_argument&) {
        return false;
    }
}

/// Compare Umbra and DuckDB results cell-by-cell.
static void diffTest(Catalog& catalog, Table& table, DuckDBOracle& duck, const std::string& sql,
                     Backend backend) {
    auto umbraRows = executeUmbra(catalog, table, sql, backend);
    auto duckRows = duck.query(sql);

    // Build string representations for diagnostics.
    auto rowsToString = [](const std::vector<std::vector<std::string>>& rows) {
        std::string s;
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) {
                    s += " | ";
                }
                s += row[i];
            }
            s += "\n";
        }
        return s;
    };

    std::string umbraStr = rowsToString(umbraRows);
    std::string duckStr = rowsToString(duckRows);
    CAPTURE(backendName(backend));
    CAPTURE(sql);
    CAPTURE(umbraStr);
    CAPTURE(duckStr);

    REQUIRE(umbraRows.size() == duckRows.size());
    for (size_t r = 0; r < umbraRows.size(); ++r) {
        REQUIRE(umbraRows[r].size() == duckRows[r].size());
        for (size_t c = 0; c < umbraRows[r].size(); ++c) {
            CHECK(umbraRows[r][c] == duckRows[r][c]);
        }
    }
}

/// Compare Umbra and DuckDB results with approximate numeric matching.
/// Use this for queries containing AVG or other operations where Umbra's integer
/// division semantics differ from DuckDB's floating-point semantics.
static void diffTestApprox(Catalog& catalog, Table& table, DuckDBOracle& duck,
                           const std::string& sql, Backend backend) {
    auto umbraRows = executeUmbra(catalog, table, sql, backend);
    auto duckRows = duck.query(sql);

    auto rowsToString = [](const std::vector<std::vector<std::string>>& rows) {
        std::string s;
        for (const auto& row : rows) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) {
                    s += " | ";
                }
                s += row[i];
            }
            s += "\n";
        }
        return s;
    };

    std::string umbraStr = rowsToString(umbraRows);
    std::string duckStr = rowsToString(duckRows);
    CAPTURE(backendName(backend));
    CAPTURE(sql);
    CAPTURE(umbraStr);
    CAPTURE(duckStr);

    REQUIRE(umbraRows.size() == duckRows.size());
    for (size_t r = 0; r < umbraRows.size(); ++r) {
        REQUIRE(umbraRows[r].size() == duckRows[r].size());
        for (size_t c = 0; c < umbraRows[r].size(); ++c) {
            CHECK(approxEqual(umbraRows[r][c], duckRows[r][c]));
        }
    }
}

// ── Differential Tests ──────────────────────────────────────────────────────

// Each test runs against all available backends.
// GENERATE produces a separate sub-case per backend.

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

TEST_CASE("Diff: simple scan", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, l_quantity FROM lineitem ORDER BY l_orderkey, l_quantity",
             backend);
}

TEST_CASE("Diff: scan with integer filter", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, l_quantity FROM lineitem "
             "WHERE l_orderkey > 1 ORDER BY l_orderkey, l_quantity",
             backend);
}

TEST_CASE("Diff: scan with decimal filter", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_quantity, l_extendedprice FROM lineitem "
             "WHERE l_quantity > 30.00 ORDER BY l_quantity",
             backend);
}

TEST_CASE("Diff: global aggregate", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck, "SELECT SUM(l_quantity), COUNT(*) FROM lineitem", backend);
}

TEST_CASE("Diff: group by CHAR(1)", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, SUM(l_quantity), COUNT(*) FROM lineitem "
             "GROUP BY l_returnflag ORDER BY l_returnflag",
             backend);
}

TEST_CASE("Diff: group by two keys", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, l_linestatus, SUM(l_quantity), COUNT(*) FROM lineitem "
             "GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus",
             backend);
}

TEST_CASE("Diff: group by integer column", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, SUM(l_quantity), COUNT(*) FROM lineitem "
             "GROUP BY l_orderkey ORDER BY l_orderkey",
             backend);
}

TEST_CASE("Diff: aggregate with filter", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, SUM(l_quantity), COUNT(*) FROM lineitem "
             "WHERE l_quantity > 20.00 "
             "GROUP BY l_returnflag ORDER BY l_returnflag",
             backend);
}

TEST_CASE("Diff: SUM of extendedprice", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, SUM(l_extendedprice) FROM lineitem "
             "GROUP BY l_returnflag ORDER BY l_returnflag",
             backend);
}

TEST_CASE("Diff: scan multiple integer columns", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, l_partkey, l_suppkey, l_linenumber "
             "FROM lineitem ORDER BY l_orderkey, l_linenumber",
             backend);
}

TEST_CASE("Diff: ORDER BY descending", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck, "SELECT l_quantity FROM lineitem ORDER BY l_quantity DESC",
             backend);
}

TEST_CASE("Diff: count with filter", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck, "SELECT COUNT(*) FROM lineitem WHERE l_discount > 0.05",
             backend);
}

TEST_CASE("Diff: multiple aggregates", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, SUM(l_quantity), SUM(l_extendedprice), COUNT(*) "
             "FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag",
             backend);
}

TEST_CASE("Diff: Q1 simple aggregates", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_returnflag, l_linestatus, "
             "SUM(l_quantity), SUM(l_extendedprice), COUNT(*) "
             "FROM lineitem "
             "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
             "GROUP BY l_returnflag, l_linestatus "
             "ORDER BY l_returnflag, l_linestatus",
             backend);
}

TEST_CASE("Diff: filter equality", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, l_quantity FROM lineitem "
             "WHERE l_orderkey = 2 ORDER BY l_quantity",
             backend);
}

TEST_CASE("Diff: filter with AND", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck,
             "SELECT l_orderkey, l_quantity FROM lineitem "
             "WHERE l_orderkey >= 2 AND l_quantity < 50.00 "
             "ORDER BY l_orderkey, l_quantity",
             backend);
}

TEST_CASE("Diff: AVG aggregate", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTestApprox(catalog, table, duck,
                   "SELECT l_returnflag, AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount) "
                   "FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag",
                   backend);
}

TEST_CASE("Diff: arithmetic expression in SELECT", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTestApprox(catalog, table, duck,
                   "SELECT l_orderkey, l_extendedprice * (1 - l_discount) "
                   "FROM lineitem ORDER BY l_orderkey, l_linenumber",
                   backend);
}

TEST_CASE("Diff: SUM of arithmetic expression", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTestApprox(catalog, table, duck,
                   "SELECT l_returnflag, SUM(l_extendedprice * (1 - l_discount)) "
                   "FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag",
                   backend);
}

TEST_CASE("Diff: SUM of nested arithmetic expression", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTestApprox(catalog, table, duck,
                   "SELECT l_returnflag, SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) "
                   "FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag",
                   backend);
}

TEST_CASE("Diff: full TPC-H Q1", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTestApprox(catalog, table, duck,
                   "SELECT l_returnflag, l_linestatus, "
                   "SUM(l_quantity), SUM(l_extendedprice), "
                   "SUM(l_extendedprice * (1 - l_discount)), "
                   "SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), "
                   "AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), "
                   "COUNT(*) "
                   "FROM lineitem "
                   "WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY "
                   "GROUP BY l_returnflag, l_linestatus "
                   "ORDER BY l_returnflag, l_linestatus",
                   backend);
}

TEST_CASE("Diff: aggregate no rows pass filter", "[sql][differential]") {
    auto backend = GENERATE(from_range(ALL_BACKENDS));
    auto catalog = makeCatalog();
    auto table = makeTable();
    DuckDBOracle duck;
    diffTest(catalog, table, duck, "SELECT COUNT(*) FROM lineitem WHERE l_orderkey > 100", backend);
}
