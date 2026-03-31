/// Isolated benchmark: why is LLVM execution in adaptive mode slower?
///
/// Scenarios (all morsel-dispatched, morselSize=10000):
///   A. LLVM-only morsel (601), from row 0                              — baseline
///   B. FS(62) → sync LLVM compile → LLVM(539)                         — original finding
///   C. Async compile → wait → LLVM-only (601)                         — async residue test
///   D. Full adaptive (FS + async LLVM → switch)                       — production path
///   E. Pre-compile both → FS(62) → LLVM(539), NO compile in between   — isolate compile gap
///   F. LLVM-only from row 620K (539 morsels, fresh HT)                — isolate row offset
///   G. LLVM-only (601) with FS JIT code alive but unused              — FS code presence

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "backend/flying_start/translator.h"
#ifdef UMBRA_LLVM_BACKEND
#include "backend/llvm/llvm_compiler.h"
#endif
#include "binder/binder.h"
#include "catalog/catalog.h"
#include "codegen/api/codegen_context.h"
#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/table_scan.h"
#include "common/types.h"
#include "execution/async_compiler.h"
#include "execution/pipeline.h"
#include "ir/program.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "storage/runtime_context.h"
#include "storage/table.h"
#include "storage/tbl_loader.h"

using namespace umbra;
using namespace umbra::execution;

namespace {

auto now() { return std::chrono::high_resolution_clock::now(); }

double ms(std::chrono::high_resolution_clock::time_point a,
          std::chrono::high_resolution_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

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

constexpr uint64_t MORSEL_SIZE = 10000;

struct BuildPipelineIR {
    ir::Program program;
    ir::FunctionId funcId{};
    std::unique_ptr<codegen::HashAggregateTranslator> hashAggTranslator;
};

BuildPipelineIR codegenBuildPipeline(const optimizer::OptimizeResult& result) {
    const auto* scanOp = static_cast<const optimizer::PhysicalTableScan*>(
        PipelineExecutor::findOperator(result.root, optimizer::PhysicalNodeKind::TABLE_SCAN));
    const auto* aggOp = static_cast<const optimizer::PhysicalHashAggregate*>(
        PipelineExecutor::findOperator(result.root, optimizer::PhysicalNodeKind::HASH_AGGREGATE));

    auto scanTranslator = std::make_unique<codegen::TableScanTranslator>(scanOp);
    auto hashAggTranslator = std::make_unique<codegen::HashAggregateTranslator>(aggOp);
    scanTranslator->setParent(hashAggTranslator.get());
    hashAggTranslator->setChild(scanTranslator.get());

    codegen::CodegenContext cg;
    codegen::setActiveContext(&cg);
    auto funcId = cg.createFunction("pipeline_build", ir::TypeId::VOID,
                                    {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
    ctx.startRow = cg.getArg<codegen::Int64>(1);
    ctx.endRow = cg.getArg<codegen::Int64>(2);
    hashAggTranslator->produce(ctx);
    cg.emitRetVoid();
    auto program = cg.finalize();
    codegen::setActiveContext(nullptr);

    return {std::move(program), funcId, std::move(hashAggTranslator)};
}

struct ExecContext {
    RuntimeContext rtCtx{};
    uint64_t ctxArg{};
};

ExecContext makeExecCtx(Table& table, codegen::HashTable* ht) {
    ExecContext ec;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    ec.rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    ec.rtCtx.rowCount = table.rowCount;
    ec.rtCtx.hashTable = ht;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ec.ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&ec.rtCtx));
    return ec;
}

void dispatchMorsels(void* jitFunc, uint64_t ctxArg, uint64_t startRow, uint64_t totalItems,
                     uint64_t morselSize) {
    uint64_t args[3] = {ctxArg, 0, 0};
    uint64_t cursor = startRow;
    while (cursor < totalItems) {
        uint64_t end = std::min(cursor + morselSize, totalItems);
        args[1] = cursor;
        args[2] = end;
        flying_start::FlyingStartCompiler::execute(jitFunc, std::span<const uint64_t>(args));
        cursor = end;
    }
}

double median(std::vector<double>& v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void dumpHT(const codegen::HashTable& ht, const char* label) {
    std::cerr << "   [" << label << "] HT size=" << ht.size() << " cap=" << ht.capacity() << "\n";
    uint32_t stride = ht.slotStride();
    for (uint32_t i = 0; i < ht.capacity(); ++i) {
        uint8_t* slot = ht.slotBuffer() + static_cast<uint64_t>(i) * stride;
        if (slot[0] == 0) continue;  // not occupied
        uint64_t hash = 0;
        std::memcpy(&hash, slot + 1, 8);
        uint8_t* payload = slot + 9;
        // First 2 bytes of payload = key1 (CHAR(1)), key2 (CHAR(1))
        std::cerr << "     slot " << i << ": hash=0x" << std::hex << hash << std::dec
                  << " key1=" << static_cast<int>(payload[0])
                  << "(" << static_cast<char>(payload[0]) << ")"
                  << " key2=" << static_cast<int>(payload[1])
                  << "(" << static_cast<char>(payload[1]) << ")\n";
    }
}

}  // namespace

#ifdef UMBRA_LLVM_BACKEND

TEST_CASE("Adaptive analysis: isolate LLVM morsel overhead", "[perf]") {
    auto lineitemDef = lineitemTableDef();
    Catalog catalog;
    catalog.addTable(lineitemDef);
    Table table;
    std::string sf1Path = "data/lineitem_sf1.tbl";
    if (std::filesystem::exists(sf1Path)) {
        table = loadTblFile(sf1Path, lineitemDef);
    } else {
        SKIP("No SF=1 data at " + sf1Path);
    }
    table.finalize();

    auto parseResult = parser::Parser::parse(Q1_SQL);
    binder::Binder binder(catalog);
    auto bindResult = binder.bind(*parseResult.stmt);
    optimizer::Optimizer opt;
    auto optResult = opt.optimize(bindResult);

    uint64_t totalItems = table.rowCount;
    uint64_t totalMorsels = (totalItems + MORSEL_SIZE - 1) / MORSEL_SIZE;
    constexpr uint64_t SWITCH_MORSEL = 62;
    uint64_t switchRow = SWITCH_MORSEL * MORSEL_SIZE;

    constexpr int RUNS = 7;

    std::cerr << "\n=== Adaptive Analysis (SF=1, " << totalItems << " rows, " << totalMorsels
              << " morsels) ===\n\n";

    // ── A: LLVM-only morsel from row 0 (baseline) ────────────────────────
    {
        std::vector<double> times;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            llvm_backend::LLVMCompiler llvmCompiler;
            void* func = llvmCompiler.compile(ir.program, ir.funcId);
            auto t0 = now();
            dispatchMorsels(func, ec.ctxArg, 0, totalItems, MORSEL_SIZE);
            times.push_back(ms(t0, now()));
            if (r == 0) {
                dumpHT(ir.hashAggTranslator->hashTable(), "A after LLVM");
            }
        }
        std::cerr << "A. LLVM-only (601m, row 0):              " << median(times) << " ms\n";
    }

    // ── B: FS(62) → sync LLVM compile → LLVM(539) ───────────────────────
    {
        std::vector<double> times;
        std::vector<double> llvmT;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto prog = std::make_shared<ir::Program>(std::move(ir.program));
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            flying_start::FlyingStartCompiler fsComp;
            void* fsFunc = fsComp.compile(*prog, ir.funcId);
            auto t0 = now();
            dispatchMorsels(fsFunc, ec.ctxArg, 0, switchRow, MORSEL_SIZE);
            auto t1 = now();
            llvm_backend::LLVMCompiler llvmComp;
            void* llvmFunc = llvmComp.compile(*prog, ir.funcId);
            auto t2 = now();
            dispatchMorsels(llvmFunc, ec.ctxArg, switchRow, totalItems, MORSEL_SIZE);
            auto t3 = now();
            llvmT.push_back(ms(t2, t3));
            times.push_back(ms(t0, t1) + ms(t2, t3));
        }
        std::cerr << "B. FS(62)→compile→LLVM(539):             " << median(times)
                  << " ms  (LLVM phase=" << median(llvmT) << ")\n";
    }

    // ── E: Pre-compile both → FS(62) → LLVM(539), NO compile in between ─
    {
        std::vector<double> times;
        std::vector<double> fsT;
        std::vector<double> llvmT;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto prog = std::make_shared<ir::Program>(std::move(ir.program));
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            flying_start::FlyingStartCompiler fsComp;
            void* fsFunc = fsComp.compile(*prog, ir.funcId);
            llvm_backend::LLVMCompiler llvmComp;
            void* llvmFunc = llvmComp.compile(*prog, ir.funcId);
            auto t0 = now();
            dispatchMorsels(fsFunc, ec.ctxArg, 0, switchRow, MORSEL_SIZE);
            auto t1 = now();
            if (r == 0) {
                dumpHT(ir.hashAggTranslator->hashTable(), "E after FS");
            }
            dispatchMorsels(llvmFunc, ec.ctxArg, switchRow, totalItems, MORSEL_SIZE);
            auto t2 = now();
            if (r == 0) {
                dumpHT(ir.hashAggTranslator->hashTable(), "E after LLVM");
            }
            fsT.push_back(ms(t0, t1));
            llvmT.push_back(ms(t1, t2));
            times.push_back(ms(t0, t2));
        }
        std::cerr << "E. Pre-compile→FS(62)→LLVM(539) no gap:  " << median(times)
                  << " ms  (FS=" << median(fsT) << ", LLVM=" << median(llvmT) << ")\n";
    }

    // ── F: LLVM-only from row 620K (539 morsels, fresh HT) ──────────────
    {
        std::vector<double> times;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            llvm_backend::LLVMCompiler llvmComp;
            void* func = llvmComp.compile(ir.program, ir.funcId);
            auto t0 = now();
            dispatchMorsels(func, ec.ctxArg, switchRow, totalItems, MORSEL_SIZE);
            times.push_back(ms(t0, now()));
        }
        std::cerr << "F. LLVM-only (539m, row 620K, fresh HT):  " << median(times) << " ms\n";
    }

    // ── G: LLVM-only (601) with FS code compiled but unused ──────────────
    {
        std::vector<double> times;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto prog = std::make_shared<ir::Program>(std::move(ir.program));
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            flying_start::FlyingStartCompiler fsComp;
            [[maybe_unused]] void* fsFunc = fsComp.compile(*prog, ir.funcId);
            llvm_backend::LLVMCompiler llvmComp;
            void* llvmFunc = llvmComp.compile(*prog, ir.funcId);
            auto t0 = now();
            dispatchMorsels(llvmFunc, ec.ctxArg, 0, totalItems, MORSEL_SIZE);
            times.push_back(ms(t0, now()));
        }
        std::cerr << "G. LLVM-only (601m) + FS compiled unused: " << median(times) << " ms\n";
    }

    // ── D: Full adaptive ─────────────────────────────────────────────────
    {
        std::vector<double> times;
        std::vector<double> fsT;
        std::vector<double> llvmT;
        std::vector<uint32_t> sw;
        for (int r = 0; r < RUNS; ++r) {
            auto ir = codegenBuildPipeline(optResult);
            auto prog = std::make_shared<ir::Program>(std::move(ir.program));
            auto ec = makeExecCtx(table, ir.hashAggTranslator->hashTablePtr());
            AsyncCompiler async;
            async.startAsync(prog, ir.funcId);
            flying_start::FlyingStartCompiler fsComp;
            void* activeFunc = fsComp.compile(*prog, ir.funcId);
            auto t0 = now();
            auto ph = t0;
            bool usingLlvm = false;
            uint64_t cursor = 0;
            uint32_t mc = 0;
            double fV = 0;
            double lV = 0;
            uint64_t args[3] = {ec.ctxArg, 0, 0};
            while (cursor < totalItems) {
                uint64_t end = std::min(cursor + MORSEL_SIZE, totalItems);
                args[1] = cursor;
                args[2] = end;
                flying_start::FlyingStartCompiler::execute(activeFunc,
                                                           std::span<const uint64_t>(args));
                cursor = end;
                ++mc;
                if (!usingLlvm && async.isReady()) {
                    auto sp = now();
                    fV = ms(ph, sp);
                    activeFunc = async.getJitFunc();
                    usingLlvm = true;
                    ph = sp;
                }
            }
            auto t1 = now();
            if (usingLlvm) {
                lV = ms(ph, t1);
            } else {
                fV = ms(ph, t1);
            }
            fsT.push_back(fV);
            llvmT.push_back(lV);
            times.push_back(ms(t0, t1));
            sw.push_back(mc - static_cast<uint32_t>((totalItems - cursor + MORSEL_SIZE - 1) /
                                                     MORSEL_SIZE));
        }
        std::sort(sw.begin(), sw.end());
        std::cerr << "D. Full adaptive:                        " << median(times)
                  << " ms  (FS=" << median(fsT) << ", LLVM=" << median(llvmT) << ")\n";
    }

    std::cerr << "\nKey comparisons:\n";
    std::cerr << "  A vs F: row offset effect (both LLVM-only, different start rows)\n";
    std::cerr << "  F vs E.LLVM: hash table state effect (fresh HT vs FS-populated HT)\n";
    std::cerr << "  E vs B: compile-gap effect (no gap vs 13ms LLVM compile between phases)\n";
    std::cerr << "  A vs G: FS code presence in memory (compiled but unused)\n";

    CHECK(true);
}

#endif
