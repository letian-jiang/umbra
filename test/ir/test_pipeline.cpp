#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "binder/binder.h"
#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "catalog/catalog.h"
#include "codegen/api/codegen_context.h"
#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/operator_translator.h"
#include "codegen/operators/projection.h"
#include "codegen/operators/result_writer.h"
#include "codegen/operators/sort.h"
#include "codegen/operators/table_scan.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/structures/hash_table.h"
#include "common/types.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "ir/opcode.h"
#include "ir/program.h"
#include "optimizer/optimizer.h"
#include "optimizer/physical_operator.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "storage/column.h"
#include "storage/runtime_context.h"
#include "storage/table.h"

using namespace umbra;
using namespace umbra::codegen;
using namespace umbra::optimizer;
using namespace umbra::binder;
using namespace umbra::execution;
using namespace umbra::ir;
using umbra::interpreter::Interpreter;

// ── Helper: create a mini lineitem-like table for aggregation testing ────────

static Catalog makeTestCatalog() {
    Catalog catalog;
    TableDef def;
    def.name = "lineitem";
    // Columns matching TPC-H lineitem (indices match TPC-H convention):
    // 0: l_orderkey (INTEGER) - unused for Q1 but keeps index alignment
    // 1: l_partkey (INTEGER) - unused
    // 2: l_suppkey (INTEGER) - unused
    // 3: l_linenumber (INTEGER) - unused
    // 4: l_quantity (DECIMAL 15,2)
    // 5: l_extendedprice (DECIMAL 15,2)
    // 6: l_discount (DECIMAL 15,2)
    // 7: l_tax (DECIMAL 15,2)
    // 8: l_returnflag (INT8 - char code)
    // 9: l_linestatus (INT8 - char code)
    // 10: l_shipdate (DATE)
    for (uint32_t i = 0; i < 11; ++i) {
        ColumnDef col;
        col.columnIndex = i;
        switch (i) {
            case 0:
            case 1:
            case 2:
            case 3:
                col.name = "col_" + std::to_string(i);
                col.type = SQLType::integer();
                break;
            case 4:
                col.name = "l_quantity";
                col.type = SQLType::decimal(15, 2);
                break;
            case 5:
                col.name = "l_extendedprice";
                col.type = SQLType::decimal(15, 2);
                break;
            case 6:
                col.name = "l_discount";
                col.type = SQLType::decimal(15, 2);
                break;
            case 7:
                col.name = "l_tax";
                col.type = SQLType::decimal(15, 2);
                break;
            case 8:
                col.name = "l_returnflag";
                col.type = SQLType{SQLTypeId::BOOLEAN};
                break;
            case 9:
                col.name = "l_linestatus";
                col.type = SQLType{SQLTypeId::BOOLEAN};
                break;
            case 10:
                col.name = "l_shipdate";
                col.type = SQLType::date();
                break;
        }
        def.columns.push_back(std::move(col));
    }
    catalog.addTable(std::move(def));
    return catalog;
}

static Table makeTestLineitemTable() {
    // Create a small table with 6 rows, 2 groups for aggregation.
    Table table;
    table.name = "lineitem";

    // Columns 0-3: unused placeholders (int32).
    for (int i = 0; i < 4; ++i) {
        Column c = Column::create(SQLType::integer());
        auto& v = std::get<std::vector<int32_t>>(c.storage);
        v = {0, 0, 0, 0, 0, 0};
        table.columns.push_back(std::move(c));
    }

    // Column 4: l_quantity (DECIMAL 15,2 → int64, scaled by 100)
    {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        // Quantities: 10.00, 20.00, 30.00, 15.00, 25.00, 35.00 → scaled by 100
        v = {1000, 2000, 3000, 1500, 2500, 3500};
        table.columns.push_back(std::move(c));
    }

    // Column 5: l_extendedprice (DECIMAL 15,2 → int64)
    {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        // Prices: 100.00, 200.00, 300.00, 150.00, 250.00, 350.00
        v = {10000, 20000, 30000, 15000, 25000, 35000};
        table.columns.push_back(std::move(c));
    }

    // Column 6: l_discount (DECIMAL 15,2 → int64)
    {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        // Discounts: 0.05, 0.10, 0.05, 0.10, 0.05, 0.10
        v = {5, 10, 5, 10, 5, 10};
        table.columns.push_back(std::move(c));
    }

    // Column 7: l_tax (DECIMAL 15,2 → int64)
    {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        // Tax: 0.08 for all
        v = {8, 8, 8, 8, 8, 8};
        table.columns.push_back(std::move(c));
    }

    // Column 8: l_returnflag (INT8 — 'A'=65 or 'N'=78)
    {
        Column c = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(c.storage);
        // Group A (65): rows 0, 1, 2; Group N (78): rows 3, 4, 5
        v = {65, 65, 65, 78, 78, 78};
        table.columns.push_back(std::move(c));
    }

    // Column 9: l_linestatus (INT8 — 'F'=70 or 'O'=79)
    {
        Column c = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(c.storage);
        // All in same linestatus 'F'=70
        v = {70, 70, 70, 70, 70, 70};
        table.columns.push_back(std::move(c));
    }

    // Column 10: l_shipdate (DATE — days since 2000-01-01)
    {
        Column c = Column::create(SQLType::date());
        auto& v = std::get<std::vector<int32_t>>(c.storage);
        // All dates before the threshold (pass filter).
        // 1998-09-02 is about -456 days before 2000-01-01
        // Let's use -500 (all pass a threshold of -400).
        v = {-500, -500, -500, -500, -500, -500};
        table.columns.push_back(std::move(c));
    }

    table.rowCount = 6;
    table.finalize();
    return table;
}

// ── Test: Build pipeline (scan + aggregate) ──────────────────────────────────

TEST_CASE("Pipeline: build pipeline aggregates correctly", "[pipeline]") {
    auto catalog = makeTestCatalog();
    auto table = makeTestLineitemTable();
    const auto* tableDef = catalog.lookupTable("lineitem");
    REQUIRE(tableDef != nullptr);

    // Create physical plan manually.
    // TableScan with filter: l_shipdate <= -400 (all 6 rows pass)
    PhysicalTableScan scan(tableDef);
    scan.columnIndices = {4, 5, 6, 7, 8, 9, 10};

    BoundColumnRef shipDateCol(0, 10, SQLType::date(), "l_shipdate");
    BoundLiteral dateThreshold(SQLType::date(), -400);
    BoundBinaryExpr filterExpr(parser::BinaryOp::LE, &shipDateCol, &dateThreshold,
                               SQLType::boolean());
    scan.filter = &filterExpr;

    // Hash aggregate.
    PhysicalHashAggregate agg(&scan);
    // Group by: l_returnflag (col 8), l_linestatus (col 9)
    BoundColumnRef flagCol(0, 8, SQLType{SQLTypeId::BOOLEAN}, "l_returnflag");
    BoundColumnRef statusCol(0, 9, SQLType{SQLTypeId::BOOLEAN}, "l_linestatus");
    agg.groupByKeys = {&flagCol, &statusCol};

    // Aggregates: SUM(l_quantity), COUNT(*)
    BoundColumnRef qtyCol(0, 4, SQLType::decimal(15, 2), "l_quantity");
    BoundAggCall sumQty(AggFunc::SUM, &qtyCol, SQLType::decimal(15, 2));
    BoundAggCall countStar(AggFunc::COUNT, nullptr, SQLType::bigint());
    agg.aggregates = {&sumQty, &countStar};

    // Create translators.
    auto scanTranslator = std::make_unique<TableScanTranslator>(&scan);
    auto aggTranslator = std::make_unique<HashAggregateTranslator>(&agg);

    scanTranslator->setParent(aggTranslator.get());
    aggTranslator->setChild(scanTranslator.get());

    // Generate IR.
    CodegenContext cg;
    setActiveContext(&cg);

    auto funcId = cg.createFunction("pipeline_build", TypeId::VOID, {TypeId::PTR});
    CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<Ptr<Int8>>(0);

    aggTranslator->produce(ctx);
    cg.emitRetVoid();

    auto program = cg.finalize();

    // Set up runtime.
    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;
    rtCtx.hashTable = aggTranslator->hashTablePtr();

    Interpreter interp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    std::vector<uint64_t> args = {ctxArg};
    interp.execute(program, funcId, std::span<const uint64_t>(args));

    setActiveContext(nullptr);

    // Verify: 2 groups (A-F and N-F).
    auto& ht = aggTranslator->hashTable();
    REQUIRE(ht.size() == 2);

    // We can verify by iterating the hash table entries.
    // Group A: rows 0,1,2 → sum_qty = 1000+2000+3000=6000, count=3
    // Group N: rows 3,4,5 → sum_qty = 1500+2500+3500=7500, count=3
    // (Note: SUM(l_quantity) stores raw int64 values, which are the DECIMAL-scaled values.)

    struct GroupResult {
        int8_t flag{0};
        int8_t status{0};
        int64_t sumQty{0};
        int64_t count{0};
    };

    std::vector<GroupResult> results;
    ht.iterate(
        [](uint8_t* entry, void* userData) {
            auto* res = static_cast<std::vector<GroupResult>*>(userData);
            GroupResult gr;
            // Key layout: [flag(1B), status(1B), pad, ...] — check TupleLayout.
            // Actually, we need to read the raw bytes according to the layout.
            // flag is INT8 (1 byte at offset 0), status is INT8 (1 byte at offset 1).
            std::memcpy(&gr.flag, entry, 1);
            std::memcpy(&gr.status, entry + 1, 1);
            // Agg states follow the key layout. Key layout has:
            //   offset 0: flag (1B)
            //   offset 1: status (1B)
            //   null bitmap at offset 2 (1B)
            //   total key size = 3
            // Agg layout:
            //   offset 0: sum_qty (8B, aligned to 8)
            //   offset 8: count (8B)
            //   null bitmap at offset 16 (1B)
            //   total agg size = 17 (aligned up)
            // So agg states start at entry + keyTotalSize.
            // Key total size = 3 (2 fields of 1B each + 1B null bitmap).
            // But wait, TupleLayout aligns. INT8 has alignment 1.
            // Fields: [0: INT8@0, 1: INT8@1], null_bitmap@2, total=3.
            // Agg layout: [0: INT64@0, 1: INT64@8], null_bitmap@16, total=17.
            // Entry = key_bytes(3) + agg_bytes(17) = 20.
            // But agg layout starts right after key layout in the entry.
            auto aggBase = entry + 3;
            std::memcpy(&gr.sumQty, aggBase, sizeof(int64_t));
            std::memcpy(&gr.count, aggBase + 8, sizeof(int64_t));
            res->push_back(gr);
        },
        &results);

    REQUIRE(results.size() == 2);

    // Sort by flag for deterministic checking.
    if (results[0].flag > results[1].flag) {
        std::swap(results[0], results[1]);
    }

    // Group A (flag=65): sum_qty=6000, count=3
    CHECK(results[0].flag == 65);
    CHECK(results[0].status == 70);
    CHECK(results[0].sumQty == 6000);
    CHECK(results[0].count == 3);

    // Group N (flag=78): sum_qty=7500, count=3
    CHECK(results[1].flag == 78);
    CHECK(results[1].status == 70);
    CHECK(results[1].sumQty == 7500);
    CHECK(results[1].count == 3);
}

// ── Test: Full two-pipeline execution via PipelineExecutor ───────────────────

TEST_CASE("Pipeline: full Q1-like two-pipeline execution", "[pipeline]") {
    auto catalog = makeTestCatalog();
    auto table = makeTestLineitemTable();

    // Use the parser + binder + optimizer to produce a physical plan.
    // But for a focused unit test, we'll build the physical plan manually.

    const auto* tableDef = catalog.lookupTable("lineitem");
    REQUIRE(tableDef != nullptr);

    // Physical plan: Sort -> Projection -> HashAggregate -> TableScan(filter)
    PhysicalTableScan scan(tableDef);
    scan.columnIndices = {4, 5, 6, 7, 8, 9, 10};

    BoundColumnRef shipDateCol(0, 10, SQLType::date(), "l_shipdate");
    BoundLiteral dateThreshold(SQLType::date(), -400);
    BoundBinaryExpr filterExpr(parser::BinaryOp::LE, &shipDateCol, &dateThreshold,
                               SQLType::boolean());
    scan.filter = &filterExpr;

    PhysicalHashAggregate agg(&scan);
    BoundColumnRef flagCol(0, 8, SQLType{SQLTypeId::BOOLEAN}, "l_returnflag");
    BoundColumnRef statusCol(0, 9, SQLType{SQLTypeId::BOOLEAN}, "l_linestatus");
    agg.groupByKeys = {&flagCol, &statusCol};

    BoundColumnRef qtyCol(0, 4, SQLType::decimal(15, 2), "l_quantity");
    BoundColumnRef priceCol(0, 5, SQLType::decimal(15, 2), "l_extendedprice");
    BoundAggCall sumQty(AggFunc::SUM, &qtyCol, SQLType::decimal(15, 2));
    BoundAggCall sumPrice(AggFunc::SUM, &priceCol, SQLType::decimal(15, 2));
    BoundAggCall countStar(AggFunc::COUNT, nullptr, SQLType::bigint());
    agg.aggregates = {&sumQty, &sumPrice, &countStar};

    // Projection: [group(0), group(1), agg(0), agg(1), agg(2)]
    // (Simplified: no AVG computation, just pass through.)
    PhysicalProjection proj(&agg);
    BoundGroupRef gref0(0, SQLType{SQLTypeId::BOOLEAN});
    BoundGroupRef gref1(1, SQLType{SQLTypeId::BOOLEAN});
    BoundAggRef aref0(0, SQLType::bigint());
    BoundAggRef aref1(1, SQLType::bigint());
    BoundAggRef aref2(2, SQLType::bigint());
    proj.expressions = {&gref0, &gref1, &aref0, &aref1, &aref2};
    proj.aliases = {"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price", "count_order"};

    // Sort by l_returnflag, l_linestatus.
    PhysicalSort sort(&proj);
    BoundGroupRef sortRef0(0, SQLType{SQLTypeId::BOOLEAN});
    BoundGroupRef sortRef1(1, SQLType{SQLTypeId::BOOLEAN});
    sort.orderBy = {{&sortRef0, true}, {&sortRef1, true}};

    // Create optimizer result.
    OptimizeResult result;
    result.root = &sort;
    result.pipelines = Optimizer::decomposePipelines(&sort);

    REQUIRE(result.pipelines.size() == 2);

    // Set up result collector.
    ResultCollector collector;
    collector.setSchema({{"l_returnflag", SQLType::fixedChar(1)},
                         {"l_linestatus", SQLType::fixedChar(1)},
                         {"sum_qty", SQLType::bigint()},
                         {"sum_base_price", SQLType::bigint()},
                         {"count_order", SQLType::bigint()}});

    // Execute.
    PipelineExecutor executor;
    executor.execute(result, table, collector);

    // Verify results: 2 groups sorted by flag then status.
    REQUIRE(collector.numRows() == 2);

    // Group A-F (flag=65, status=70): sum_qty=6000, sum_price=60000, count=3
    CHECK(collector.rows()[0].values[0] == 65);     // l_returnflag = 'A'
    CHECK(collector.rows()[0].values[1] == 70);     // l_linestatus = 'F'
    CHECK(collector.rows()[0].values[2] == 6000);   // sum_qty
    CHECK(collector.rows()[0].values[3] == 60000);  // sum_base_price
    CHECK(collector.rows()[0].values[4] == 3);      // count

    // Group N-F (flag=78, status=70): sum_qty=7500, sum_price=75000, count=3
    CHECK(collector.rows()[1].values[0] == 78);     // l_returnflag = 'N'
    CHECK(collector.rows()[1].values[1] == 70);     // l_linestatus = 'F'
    CHECK(collector.rows()[1].values[2] == 7500);   // sum_qty
    CHECK(collector.rows()[1].values[3] == 75000);  // sum_base_price
    CHECK(collector.rows()[1].values[4] == 3);      // count
}

// ── Test: Pipeline with single group ─────────────────────────────────────────

TEST_CASE("Pipeline: all rows in single group", "[pipeline]") {
    // Create a table where all rows have the same group-by key.
    auto catalog = makeTestCatalog();
    const auto* tableDef = catalog.lookupTable("lineitem");
    REQUIRE(tableDef != nullptr);

    Table table;
    table.name = "lineitem";

    // Columns 0-3: placeholders.
    for (int i = 0; i < 4; ++i) {
        Column c = Column::create(SQLType::integer());
        auto& v = std::get<std::vector<int32_t>>(c.storage);
        v = {0, 0, 0};
        table.columns.push_back(std::move(c));
    }

    // Column 4: l_quantity
    {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        v = {1000, 2000, 3000};  // 10.00, 20.00, 30.00
        table.columns.push_back(std::move(c));
    }

    // Columns 5,6,7: decimal placeholders.
    for (int i = 0; i < 3; ++i) {
        Column c = Column::create(SQLType::decimal(15, 2));
        auto& v = std::get<std::vector<int64_t>>(c.storage);
        v = {0, 0, 0};
        table.columns.push_back(std::move(c));
    }

    // Column 8: l_returnflag (all same: 'A'=65)
    {
        Column c = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(c.storage);
        v = {65, 65, 65};
        table.columns.push_back(std::move(c));
    }

    // Column 9: l_linestatus (all same: 'F'=70)
    {
        Column c = Column::create(SQLType::fixedChar(1));
        auto& v = std::get<std::vector<int8_t>>(c.storage);
        v = {70, 70, 70};
        table.columns.push_back(std::move(c));
    }

    // Column 10: l_shipdate (all pass filter)
    {
        Column c = Column::create(SQLType::date());
        auto& v = std::get<std::vector<int32_t>>(c.storage);
        v = {-500, -500, -500};
        table.columns.push_back(std::move(c));
    }

    table.rowCount = 3;
    table.finalize();

    // Build physical plan.
    PhysicalTableScan physScan(tableDef);
    physScan.columnIndices = {4, 8, 9, 10};

    BoundColumnRef shipDateCol(0, 10, SQLType::date(), "l_shipdate");
    BoundLiteral dateThreshold(SQLType::date(), -400);
    BoundBinaryExpr filterExpr(parser::BinaryOp::LE, &shipDateCol, &dateThreshold,
                               SQLType::boolean());
    physScan.filter = &filterExpr;

    PhysicalHashAggregate physAgg(&physScan);
    BoundColumnRef flagCol(0, 8, SQLType{SQLTypeId::BOOLEAN}, "l_returnflag");
    BoundColumnRef statusCol(0, 9, SQLType{SQLTypeId::BOOLEAN}, "l_linestatus");
    physAgg.groupByKeys = {&flagCol, &statusCol};

    BoundColumnRef qtyCol(0, 4, SQLType::decimal(15, 2), "l_quantity");
    BoundAggCall sumQty(AggFunc::SUM, &qtyCol, SQLType::decimal(15, 2));
    BoundAggCall countStar(AggFunc::COUNT, nullptr, SQLType::bigint());
    physAgg.aggregates = {&sumQty, &countStar};

    // Projection: pass-through group keys and agg results.
    PhysicalProjection physProj(&physAgg);
    BoundGroupRef gref0(0, SQLType{SQLTypeId::BOOLEAN});
    BoundGroupRef gref1(1, SQLType{SQLTypeId::BOOLEAN});
    BoundAggRef aref0(0, SQLType::bigint());
    BoundAggRef aref1(1, SQLType::bigint());
    physProj.expressions = {&gref0, &gref1, &aref0, &aref1};
    physProj.aliases = {"flag", "status", "sum_qty", "count"};

    PhysicalSort physSort(&physProj);
    BoundGroupRef sortRef0(0, SQLType{SQLTypeId::BOOLEAN});
    physSort.orderBy = {{&sortRef0, true}};

    OptimizeResult optResult;
    optResult.root = &physSort;
    optResult.pipelines = Optimizer::decomposePipelines(&physSort);

    ResultCollector collector;
    collector.setSchema({{"flag", SQLType{SQLTypeId::BOOLEAN}},
                         {"status", SQLType{SQLTypeId::BOOLEAN}},
                         {"sum_qty", SQLType::bigint()},
                         {"count", SQLType::bigint()}});

    PipelineExecutor executor;
    executor.execute(optResult, table, collector);

    // Should be 1 group.
    REQUIRE(collector.numRows() == 1);
    CHECK(collector.rows()[0].values[0] == 65);    // flag = 'A'
    CHECK(collector.rows()[0].values[1] == 70);    // status = 'F'
    CHECK(collector.rows()[0].values[2] == 6000);  // sum_qty = 10+20+30=60.00 (scaled)
    CHECK(collector.rows()[0].values[3] == 3);     // count = 3
}

// ── Test: Pipeline with no matching rows (empty result) ──────────────────────

TEST_CASE("Pipeline: filter rejects all rows", "[pipeline]") {
    // Simple table for this test.
    TableDef simpleTableDef;
    simpleTableDef.name = "test_table";
    simpleTableDef.columns.push_back({"quantity", SQLType::integer(), 0});
    simpleTableDef.columns.push_back({"price", SQLType::bigint(), 1});

    Table simpleTable;
    simpleTable.name = "test_table";
    Column c0 = Column::create(SQLType::integer());
    std::get<std::vector<int32_t>>(c0.storage) = {10, 20, 30, 40, 50};
    Column c1 = Column::create(SQLType::bigint());
    std::get<std::vector<int64_t>>(c1.storage) = {100, 200, 300, 400, 500};
    simpleTable.columns = {std::move(c0), std::move(c1)};
    simpleTable.rowCount = 5;
    simpleTable.finalize();

    // Filter: quantity > 999 (no row passes)
    BoundColumnRef colRef(0, 0, SQLType::integer(), "quantity");
    BoundLiteral litVal(SQLType::integer(), 999);
    BoundBinaryExpr filter(parser::BinaryOp::GT, &colRef, &litVal, SQLType::boolean());

    PhysicalTableScan scan(&simpleTableDef);
    scan.columnIndices = {0, 1};
    scan.filter = &filter;

    auto scanTranslator = std::make_unique<TableScanTranslator>(&scan);
    auto resultWriter = std::make_unique<ResultWriterTranslator>(2);

    scanTranslator->setParent(resultWriter.get());
    resultWriter->setChild(scanTranslator.get());

    CodegenContext cg;
    setActiveContext(&cg);

    auto funcId = cg.createFunction("test_scan_filter", TypeId::VOID, {TypeId::PTR});
    CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<Ptr<Int8>>(0);

    resultWriter->produce(ctx);
    cg.emitRetVoid();

    auto program = cg.finalize();

    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(simpleTable.columnPtrs());
    rtCtx.rowCount = simpleTable.rowCount;

    std::vector<int64_t> resultBuffer(5 * 2, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    rtCtx.resultBuffer = reinterpret_cast<void*>(resultBuffer.data());
    rtCtx.resultRowCount = 0;

    Interpreter interp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    std::vector<uint64_t> args = {ctxArg};
    interp.execute(program, funcId, std::span<const uint64_t>(args));

    setActiveContext(nullptr);

    // No rows should pass the filter.
    REQUIRE(rtCtx.resultRowCount == 0);
}
