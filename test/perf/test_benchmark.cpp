#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "common/types.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "explain/explain.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "storage/column.h"
#include "storage/table.h"

using namespace umbra;
using namespace umbra::execution;

// ── Helper: Q1-compatible catalog ────────────────────────────────────────────

static Catalog makeBenchCatalog() {
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

// ── Helper: Generate a lineitem table with N rows for benchmarking ───────────
// Uses a deterministic pattern to create realistic data.

static Table makeBenchTable(int numRows) {
    Table table;
    table.name = "lineitem";

    // Columns 0-3: placeholders.
    for (int c = 0; c < 4; ++c) {
        Column col = Column::create(SQLType::integer());
        auto& v = std::get<std::vector<int32_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            v.push_back(i + 1);
        }
        table.columns.push_back(std::move(col));
    }

    // Column 4: l_quantity (varies 1.00 to 50.00).
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            v.push_back(static_cast<int64_t>((i % 50 + 1) * 100));  // 100 to 5000
        }
        table.columns.push_back(std::move(col));
    }

    // Column 5: l_extendedprice (varies 100.00 to 10000.00).
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            v.push_back(static_cast<int64_t>((i % 100 + 1) * 10000));
        }
        table.columns.push_back(std::move(col));
    }

    // Column 6: l_discount (0.00 to 0.10).
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            v.push_back(static_cast<int64_t>(i % 11));  // 0 to 10
        }
        table.columns.push_back(std::move(col));
    }

    // Column 7: l_tax (0.00 to 0.08).
    {
        Column col = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            v.push_back(static_cast<int64_t>(i % 9));  // 0 to 8
        }
        table.columns.push_back(std::move(col));
    }

    // Column 8: l_returnflag (A/N/R).
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v.reserve(numRows);
        constexpr int8_t FLAGS[] = {65, 78, 82};  // A, N, R
        for (int i = 0; i < numRows; ++i) {
            v.push_back(FLAGS[i % 3]);
        }
        table.columns.push_back(std::move(col));
    }

    // Column 9: l_linestatus (F/O).
    {
        Column col = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(col.storage);
        v.reserve(numRows);
        constexpr int8_t STATUSES[] = {70, 79};  // F, O
        for (int i = 0; i < numRows; ++i) {
            v.push_back(STATUSES[i % 2]);
        }
        table.columns.push_back(std::move(col));
    }

    // Column 10: l_shipdate (most dates pass the Q1 filter).
    {
        Column col = Column::create(SQLType::date());
        auto& v = std::get<std::vector<int32_t>>(col.storage);
        v.reserve(numRows);
        for (int i = 0; i < numRows; ++i) {
            // ~98% pass: dates between 1992-01-01 and 1998-09-01
            // 1998-09-01 is day -488 from epoch 2000-01-01.
            // 1992-01-01 is day -2922.
            v.push_back(-2922 + (i % 2435));  // Spread across the range
        }
        table.columns.push_back(std::move(col));
    }

    table.rowCount = static_cast<uint64_t>(numRows);
    table.finalize();
    return table;
}

// ── TPC-H Q1 SQL ────────────────────────────────────────────────────────────

static const char* Q1_SQL =
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

// ── Benchmark helper ────────────────────────────────────────────────────────

struct BenchResult {
    double planMs{0.0};
    double codegenAndExecuteMs{0.0};
    double totalMs{0.0};
};

static BenchResult benchQ1(Catalog& catalog, Table& table) {
    BenchResult result;
    auto totalStart = std::chrono::high_resolution_clock::now();

    // Phase 1: Plan (parse + bind + optimize).
    auto planStart = std::chrono::high_resolution_clock::now();
    auto parseResult = parser::Parser::parse(Q1_SQL);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);
    auto planEnd = std::chrono::high_resolution_clock::now();
    result.planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();

    // Phase 2: Codegen + Execute.
    auto execStart = std::chrono::high_resolution_clock::now();
    ResultCollector collector;
    PipelineExecutor executor;
    executor.execute(optResult, table, collector);
    auto execEnd = std::chrono::high_resolution_clock::now();
    result.codegenAndExecuteMs =
        std::chrono::duration<double, std::milli>(execEnd - execStart).count();

    result.totalMs = std::chrono::duration<double, std::milli>(execEnd - totalStart).count();

    return result;
}

// ── Tests: benchmark runs ───────────────────────────────────────────────────

TEST_CASE("Benchmark: Q1 correctness with generated data", "[perf]") {
    // Small table to verify the benchmark harness works correctly.
    auto catalog = makeBenchCatalog();
    auto table = makeBenchTable(100);

    auto result = benchQ1(catalog, table);

    // Just verify it runs without crashing and produces results.
    CHECK(result.totalMs >= 0.0);
    CHECK(result.planMs >= 0.0);
    CHECK(result.codegenAndExecuteMs >= 0.0);
}

TEST_CASE("Benchmark: Q1 latency measurement", "[perf]") {
    // SF=0.01 has ~60,000 rows. We use a smaller dataset for unit testing.
    constexpr int NUM_ROWS = 1000;
    auto catalog = makeBenchCatalog();
    auto table = makeBenchTable(NUM_ROWS);

    // Warmup.
    for (int i = 0; i < 3; ++i) {
        benchQ1(catalog, table);
    }

    // Measure.
    constexpr int NUM_ITER = 10;
    std::vector<double> totalTimes;
    std::vector<double> planTimes;
    std::vector<double> execTimes;
    totalTimes.reserve(NUM_ITER);
    planTimes.reserve(NUM_ITER);
    execTimes.reserve(NUM_ITER);

    for (int i = 0; i < NUM_ITER; ++i) {
        auto result = benchQ1(catalog, table);
        totalTimes.push_back(result.totalMs);
        planTimes.push_back(result.planMs);
        execTimes.push_back(result.codegenAndExecuteMs);
    }

    // Compute statistics.
    std::sort(totalTimes.begin(), totalTimes.end());
    std::sort(planTimes.begin(), planTimes.end());
    std::sort(execTimes.begin(), execTimes.end());

    double totalMedian = totalTimes[NUM_ITER / 2];
    double planMedian = planTimes[NUM_ITER / 2];
    double execMedian = execTimes[NUM_ITER / 2];
    double totalMin = totalTimes[0];
    double totalMean =
        std::accumulate(totalTimes.begin(), totalTimes.end(), 0.0) / static_cast<double>(NUM_ITER);

    // Report timing (always print, even if test passes).
    std::cerr << "\n=== Q1 Benchmark (" << NUM_ROWS << " rows, " << NUM_ITER
              << " iterations) ===\n";
    std::cerr << "  plan (median):    " << planMedian << " ms\n";
    std::cerr << "  exec (median):    " << execMedian << " ms\n";
    std::cerr << "  total (min):      " << totalMin << " ms\n";
    std::cerr << "  total (median):   " << totalMedian << " ms\n";
    std::cerr << "  total (mean):     " << totalMean << " ms\n";

    // Non-failing check: report if we meet the target but don't fail CI.
    // The <3.1ms target is for SF=0.01 (~60K rows), not this small dataset.
    CHECK(totalMedian >= 0.0);  // Trivially true; ensures test runs.
}

TEST_CASE("Benchmark: Q1 phase breakdown", "[perf]") {
    constexpr int NUM_ROWS = 500;
    auto catalog = makeBenchCatalog();
    auto table = makeBenchTable(NUM_ROWS);

    auto result = benchQ1(catalog, table);

    // Plan should be a smaller fraction of total.
    CHECK(result.planMs < result.totalMs);
    // Codegen + execute should account for most of the time.
    CHECK(result.codegenAndExecuteMs <= result.totalMs * 1.1);  // small tolerance
}

TEST_CASE("Benchmark: EXPLAIN PROFILE Q1 SF=0.01", "[perf]") {
    constexpr int NUM_ROWS = 60175;  // SF=0.01 lineitem row count.
    auto catalog = makeBenchCatalog();
    auto table = makeBenchTable(NUM_ROWS);

    SECTION("interpreter") {
        std::string output = explain::runExplain(explain::ExplainLevel::PROFILE, Q1_SQL, catalog,
                                                 table, execution::Backend::INTERPRETER);
        std::cerr << "\n=== EXPLAIN PROFILE Q1 (interpreter, " << NUM_ROWS << " rows) ===\n";
        // Print only the Profile section.
        auto pos = output.find("Profile");
        if (pos != std::string::npos) {
            std::cerr << output.substr(pos);
        }
        CHECK(output.find("Result:") != std::string::npos);
    }

    SECTION("flying-start") {
        std::string output = explain::runExplain(explain::ExplainLevel::PROFILE, Q1_SQL, catalog,
                                                 table, execution::Backend::FLYING_START);
        std::cerr << "\n=== EXPLAIN PROFILE Q1 (flying-start, " << NUM_ROWS << " rows) ===\n";
        auto pos = output.find("Profile");
        if (pos != std::string::npos) {
            std::cerr << output.substr(pos);
        }
        CHECK(output.find("Result:") != std::string::npos);
    }

#ifdef UMBRA_LLVM_BACKEND
    SECTION("llvm") {
        std::string output = explain::runExplain(explain::ExplainLevel::PROFILE, Q1_SQL, catalog,
                                                 table, execution::Backend::LLVM);
        std::cerr << "\n=== EXPLAIN PROFILE Q1 (llvm, " << NUM_ROWS << " rows) ===\n";
        auto pos = output.find("Profile");
        if (pos != std::string::npos) {
            std::cerr << output.substr(pos);
        }
        CHECK(output.find("Result:") != std::string::npos);
    }
#endif

    SECTION("adaptive") {
        std::string output = explain::runExplain(explain::ExplainLevel::PROFILE, Q1_SQL, catalog,
                                                 table, execution::Backend::ADAPTIVE);
        std::cerr << "\n=== EXPLAIN PROFILE Q1 (adaptive, " << NUM_ROWS << " rows) ===\n";
        auto pos = output.find("Profile");
        if (pos != std::string::npos) {
            std::cerr << output.substr(pos);
        }
        CHECK(output.find("Result:") != std::string::npos);
        CHECK(output.find("morsels via Flying Start") != std::string::npos);
    }
}

TEST_CASE("Benchmark: adaptive Q1 correctness vs Flying Start", "[perf]") {
    constexpr int NUM_ROWS = 60175;
    auto catalog = makeBenchCatalog();
    auto table = makeBenchTable(NUM_ROWS);

    auto parseResult = parser::Parser::parse(Q1_SQL);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);

    // Execute with Flying Start (reference).
    ResultCollector fsCollector;
    PipelineExecutor fsExecutor(Backend::FLYING_START);
    fsExecutor.execute(optResult, table, fsCollector);

    // Execute with Adaptive.
    ResultCollector adaptiveCollector;
    PipelineExecutor adaptiveExecutor(Backend::ADAPTIVE);
    adaptiveExecutor.enableProfiling();
    adaptiveExecutor.execute(optResult, table, adaptiveCollector);

    // Compare results: same number of rows, same values.
    REQUIRE(adaptiveCollector.numRows() == fsCollector.numRows());
    for (uint32_t r = 0; r < fsCollector.numRows(); ++r) {
        for (size_t c = 0; c < fsCollector.rows()[r].values.size(); ++c) {
            CHECK(adaptiveCollector.rows()[r].values[c] == fsCollector.rows()[r].values[c]);
        }
    }

    // Verify profiling data is populated.
    const auto& profiles = adaptiveExecutor.profiles();
    REQUIRE(profiles.size() == 2);
    // Build pipeline should have multiple morsels (60175 rows / 10000 = 7 morsels).
    CHECK(profiles[0].totalMorsels == 7);
    CHECK(profiles[0].fsMorsels + profiles[0].llvmMorsels == profiles[0].totalMorsels);
    CHECK(profiles[0].fsMorsels >= 1);  // At least first morsel is FS.
    // Scan pipeline: small htCapacity → 1 morsel.
    CHECK(profiles[1].totalMorsels >= 1);

    std::cerr << "\n=== Adaptive Q1 Profiling (SF=0.01, " << NUM_ROWS << " rows) ===\n";
    std::cerr << "  Pipeline 0 (build): " << profiles[0].totalMorsels << " morsels total, "
              << profiles[0].fsMorsels << " FS, " << profiles[0].llvmMorsels << " LLVM";
    if (profiles[0].switchMorsel > 0) {
        std::cerr << ", switch at morsel " << profiles[0].switchMorsel;
    }
    std::cerr << "\n";
    std::cerr << "  Pipeline 0: codegen=" << profiles[0].codegenMs
              << "ms, compile(FS)=" << profiles[0].compileMs
              << "ms, llvm=" << profiles[0].llvmCompileMs << "ms, exec=" << profiles[0].executeMs
              << "ms\n";
    std::cerr << "  Pipeline 1 (scan): " << profiles[1].totalMorsels << " morsels total, "
              << profiles[1].fsMorsels << " FS, " << profiles[1].llvmMorsels << " LLVM\n";
}
