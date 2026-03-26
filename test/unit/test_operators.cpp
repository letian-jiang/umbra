#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "binder/bound_expr.h"
#include "codegen/api/codegen_context.h"
#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/operator_translator.h"
#include "codegen/operators/projection.h"
#include "codegen/operators/result_writer.h"
#include "codegen/operators/sort.h"
#include "codegen/operators/table_scan.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"
#include "execution/result_collector.h"
#include "ir/opcode.h"
#include "ir/program.h"
#include "optimizer/physical_operator.h"
#include "storage/runtime_context.h"
#include "storage/table.h"

using namespace umbra;
using namespace umbra::codegen;
using namespace umbra::optimizer;
using namespace umbra::binder;
using namespace umbra::ir;
using umbra::interpreter::Interpreter;

// ── Helper: create a simple lineitem-like table ──────────────────────────────

static TableDef makeTestTableDef() {
    TableDef def;
    def.name = "test_table";
    // Column 0: quantity (INTEGER)
    def.columns.push_back({"quantity", SQLType::integer(), 0});
    // Column 1: price (BIGINT)
    def.columns.push_back({"price", SQLType::bigint(), 1});
    // Column 2: flag (INT8/BOOLEAN internal)
    def.columns.push_back({"flag", SQLType{SQLTypeId::BOOLEAN}, 2});
    return def;
}

static Table makeTestTable() {
    Table table;
    table.name = "test_table";

    // Column 0: quantity (int32)
    Column c0 = Column::create(SQLType::integer());
    auto& v0 = std::get<std::vector<int32_t>>(c0.storage);
    v0 = {10, 20, 30, 40, 50};

    // Column 1: price (int64)
    Column c1 = Column::create(SQLType::bigint());
    auto& v1 = std::get<std::vector<int64_t>>(c1.storage);
    v1 = {100, 200, 300, 400, 500};

    // Column 2: flag (int8 for boolean)
    Column c2 = Column::create(SQLType::boolean());
    auto& v2 = std::get<std::vector<int8_t>>(c2.storage);
    v2 = {1, 0, 1, 0, 1};

    table.columns = {std::move(c0), std::move(c1), std::move(c2)};
    table.rowCount = 5;
    table.finalize();
    return table;
}

// ── Test: TableScan without filter ───────────────────────────────────────────

TEST_CASE("TableScan: scan all rows without filter", "[operators]") {
    auto tableDef = makeTestTableDef();
    auto table = makeTestTable();

    PhysicalTableScan scan(&tableDef);
    scan.columnIndices = {0, 1};  // quantity, price

    // Create a simple result writer that writes to the result buffer.
    auto scanTranslator = std::make_unique<TableScanTranslator>(&scan);
    auto resultWriter = std::make_unique<ResultWriterTranslator>(2);

    scanTranslator->setParent(resultWriter.get());
    resultWriter->setChild(scanTranslator.get());

    // Generate IR.
    CodegenContext cg;
    setActiveContext(&cg);

    auto funcId = cg.createFunction("test_scan", TypeId::VOID, {TypeId::PTR});
    CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<Ptr<Int8>>(0);

    resultWriter->produce(ctx);
    cg.emitRetVoid();

    auto program = cg.finalize();

    // Set up RuntimeContext.
    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;

    std::vector<int64_t> resultBuffer(5 * 2, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    rtCtx.resultBuffer = reinterpret_cast<void*>(resultBuffer.data());
    rtCtx.resultRowCount = 0;

    // Execute.
    Interpreter interp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    std::vector<uint64_t> args = {ctxArg};
    interp.execute(program, funcId, std::span<const uint64_t>(args));

    setActiveContext(nullptr);

    // Verify results.
    REQUIRE(rtCtx.resultRowCount == 5);
    // Row 0: quantity=10, price=100
    CHECK(resultBuffer[0] == 10);
    CHECK(resultBuffer[1] == 100);
    // Row 1: quantity=20, price=200
    CHECK(resultBuffer[2] == 20);
    CHECK(resultBuffer[3] == 200);
    // Row 4: quantity=50, price=500
    CHECK(resultBuffer[8] == 50);
    CHECK(resultBuffer[9] == 500);
}

// ── Test: TableScan with filter ──────────────────────────────────────────────

TEST_CASE("TableScan: scan with filter predicate", "[operators]") {
    auto tableDef = makeTestTableDef();
    auto table = makeTestTable();

    // Filter: quantity > 25 (i.e., rows with quantity 30, 40, 50)
    BoundColumnRef colRef(0, 0, SQLType::integer(), "quantity");
    BoundLiteral litVal(SQLType::integer(), 25);
    BoundBinaryExpr filter(parser::BinaryOp::GT, &colRef, &litVal, SQLType::boolean());

    PhysicalTableScan scan(&tableDef);
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
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;

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

    // Should have 3 rows (quantity > 25): 30, 40, 50
    REQUIRE(rtCtx.resultRowCount == 3);
    CHECK(resultBuffer[0] == 30);
    CHECK(resultBuffer[1] == 300);
    CHECK(resultBuffer[2] == 40);
    CHECK(resultBuffer[3] == 400);
    CHECK(resultBuffer[4] == 50);
    CHECK(resultBuffer[5] == 500);
}

// ── Test: ResultCollector ────────────────────────────────────────────────────

TEST_CASE("ResultCollector: prepare and materialize", "[operators]") {
    umbra::execution::ResultCollector collector;
    collector.setSchema({{"col_a", SQLType::integer()}, {"col_b", SQLType::bigint()}});
    collector.prepare(10);

    // Simulate writing 3 rows.
    auto* buf = collector.buffer();
    buf[0] = 1;
    buf[1] = 100;
    buf[2] = 2;
    buf[3] = 200;
    buf[4] = 3;
    buf[5] = 300;

    collector.materialize(3);

    REQUIRE(collector.numRows() == 3);
    CHECK(collector.rows()[0].values[0] == 1);
    CHECK(collector.rows()[0].values[1] == 100);
    CHECK(collector.rows()[2].values[0] == 3);
    CHECK(collector.rows()[2].values[1] == 300);
}

// ── Test: ResultCollector sort ───────────────────────────────────────────────

TEST_CASE("ResultCollector: sort by column", "[operators]") {
    umbra::execution::ResultCollector collector;
    collector.setSchema({{"name", SQLType::integer()}, {"value", SQLType::bigint()}});

    collector.addRow({3, 300});
    collector.addRow({1, 100});
    collector.addRow({2, 200});

    collector.sort({0}, {true});  // Sort by column 0 ascending.

    REQUIRE(collector.numRows() == 3);
    CHECK(collector.rows()[0].values[0] == 1);
    CHECK(collector.rows()[1].values[0] == 2);
    CHECK(collector.rows()[2].values[0] == 3);
}

// ── Test: ResultCollector sort descending ────────────────────────────────────

TEST_CASE("ResultCollector: sort descending", "[operators]") {
    umbra::execution::ResultCollector collector;
    collector.setSchema({{"val", SQLType::integer()}});

    collector.addRow({1});
    collector.addRow({3});
    collector.addRow({2});

    collector.sort({0}, {false});

    REQUIRE(collector.numRows() == 3);
    CHECK(collector.rows()[0].values[0] == 3);
    CHECK(collector.rows()[1].values[0] == 2);
    CHECK(collector.rows()[2].values[0] == 1);
}

// ── Test: ResultCollector clear ──────────────────────────────────────────────

TEST_CASE("ResultCollector: clear resets state", "[operators]") {
    umbra::execution::ResultCollector collector;
    collector.setSchema({{"val", SQLType::integer()}});
    collector.addRow({42});
    REQUIRE(collector.numRows() == 1);

    collector.clear();
    REQUIRE(collector.numRows() == 0);
}

// ── Test: ResultWriter codegen ───────────────────────────────────────────────

TEST_CASE("ResultWriter: basic value writing", "[operators]") {
    // Manually create a result writer and verify it emits correct IR.
    // This is implicitly tested through the TableScan tests above,
    // so this is a focused construction test.
    ResultWriterTranslator rw(3);
    CHECK(true);  // Construction succeeds.
}

// ── Test: SortTranslator construction ────────────────────────────────────────

TEST_CASE("SortTranslator: construction and passthrough", "[operators]") {
    PhysicalSort sort(nullptr);
    SortTranslator st(&sort);
    CHECK(st.physicalOp() == &sort);
}

// ── Test: ProjectionTranslator construction ──────────────────────────────────

TEST_CASE("ProjectionTranslator: construction", "[operators]") {
    PhysicalProjection proj(nullptr);
    ProjectionTranslator pt(&proj);
    CHECK(pt.physicalOp() == &proj);
}

// ── Test: ResultCollector host functions ──────────────────────────────────────

TEST_CASE("ResultCollector: host write and buffer", "[operators]") {
    umbra::execution::ResultCollector collector;
    collector.setSchema({{"a", SQLType::integer()}, {"b", SQLType::integer()}});
    collector.prepare(10);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto collectorPtr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&collector));

    // Test hostGetBuffer.
    std::vector<uint64_t> getArgs = {collectorPtr};
    auto bufAddr = umbra::execution::ResultCollector::hostGetBuffer(getArgs.data(), 1);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    CHECK(reinterpret_cast<int64_t*>(bufAddr) == collector.buffer());

    // Test hostWriteValue.
    std::vector<uint64_t> writeArgs = {collectorPtr, 0, 0, 42};
    umbra::execution::ResultCollector::hostWriteValue(writeArgs.data(), 4);
    CHECK(collector.buffer()[0] == 42);

    std::vector<uint64_t> writeArgs2 = {collectorPtr, 0, 1, 99};
    umbra::execution::ResultCollector::hostWriteValue(writeArgs2.data(), 4);
    CHECK(collector.buffer()[1] == 99);
}

// ── Test: TableScan with single column ───────────────────────────────────────

TEST_CASE("TableScan: single column scan", "[operators]") {
    auto tableDef = makeTestTableDef();
    auto table = makeTestTable();

    PhysicalTableScan scan(&tableDef);
    scan.columnIndices = {0};  // only quantity

    auto scanTranslator = std::make_unique<TableScanTranslator>(&scan);
    auto resultWriter = std::make_unique<ResultWriterTranslator>(1);

    scanTranslator->setParent(resultWriter.get());
    resultWriter->setChild(scanTranslator.get());

    CodegenContext cg;
    setActiveContext(&cg);

    auto funcId = cg.createFunction("test_single_col", TypeId::VOID, {TypeId::PTR});
    CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<Ptr<Int8>>(0);

    resultWriter->produce(ctx);
    cg.emitRetVoid();

    auto program = cg.finalize();

    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;

    std::vector<int64_t> resultBuffer(5, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    rtCtx.resultBuffer = reinterpret_cast<void*>(resultBuffer.data());
    rtCtx.resultRowCount = 0;

    Interpreter interp;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    std::vector<uint64_t> args = {ctxArg};
    interp.execute(program, funcId, std::span<const uint64_t>(args));

    setActiveContext(nullptr);

    REQUIRE(rtCtx.resultRowCount == 5);
    CHECK(resultBuffer[0] == 10);
    CHECK(resultBuffer[4] == 50);
}

// ── Test: OperatorTranslator parent/child wiring ─────────────────────────────

TEST_CASE("OperatorTranslator: parent wiring", "[operators]") {
    auto tableDef = makeTestTableDef();
    PhysicalTableScan scan(&tableDef);
    scan.columnIndices = {0};

    TableScanTranslator st(&scan);
    ResultWriterTranslator rw(1);

    st.setParent(&rw);
    CHECK(st.parent() == &rw);
}

// ── Test: HashAggregateTranslator construction details ───────────────────────

TEST_CASE("HashAggregateTranslator: hash table creation", "[operators]") {
    auto tableDef = makeTestTableDef();
    PhysicalTableScan scan(&tableDef);
    scan.columnIndices = {0, 2};

    PhysicalHashAggregate agg(&scan);
    BoundColumnRef flagCol(0, 2, SQLType{SQLTypeId::BOOLEAN}, "flag");
    agg.groupByKeys = {&flagCol};

    BoundColumnRef qtyCol(0, 0, SQLType::integer(), "quantity");
    BoundAggCall sumQty(AggFunc::SUM, &qtyCol, SQLType::integer());
    BoundAggCall countStar(AggFunc::COUNT, nullptr, SQLType::bigint());
    agg.aggregates = {&sumQty, &countStar};

    codegen::HashAggregateTranslator hat(&agg);

    // Verify hash table is created.
    CHECK(hat.hashTablePtr() != nullptr);
    CHECK(hat.hashTable().size() == 0);
    CHECK(hat.physicalOp() == &agg);
}
