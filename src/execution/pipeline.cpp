#include "execution/pipeline.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "backend/flying_start/translator.h"
#include "backend/interpreter/interpreter.h"
#ifdef UMBRA_LLVM_BACKEND
#include "backend/llvm/llvm_compiler.h"
#endif
#include "codegen/api/codegen_context.h"
#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/projection.h"
#include "codegen/operators/result_writer.h"
#include "codegen/operators/sort.h"
#include "codegen/operators/table_scan.h"
#include "codegen/sql_values/sql_value.h"
#include "execution/async_compiler.h"
#include "ir/opcode.h"
#include "ir/printer.h"
#include "ir/program.h"
#include "optimizer/physical_operator.h"
#include "storage/runtime_context.h"

namespace umbra::execution {

namespace {

/// Helper: current time point.
auto now() {
    return std::chrono::high_resolution_clock::now();
}

/// Helper: elapsed milliseconds between two time points.
double elapsedMs(std::chrono::high_resolution_clock::time_point start,
                 std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

// ── Shared helpers ───────────────────────────────────────────────────────────

PipelineExecutor::OutputChain PipelineExecutor::buildOutputChain(
    const optimizer::OptimizeResult& result, uint32_t numCols) {
    const auto* projOp = static_cast<const optimizer::PhysicalProjection*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::PROJECTION));
    const auto* sortOp = static_cast<const optimizer::PhysicalSort*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::SORT));

    OutputChain chain;
    chain.resultWriter = std::make_unique<codegen::ResultWriterTranslator>(numCols);
    chain.bottom = chain.resultWriter.get();

    if (sortOp != nullptr) {
        chain.sortTranslator = std::make_unique<codegen::SortTranslator>(sortOp);
        chain.sortTranslator->setParent(chain.bottom);
        chain.resultWriter->setChild(chain.sortTranslator.get());
        chain.bottom = chain.sortTranslator.get();
    }

    if (projOp != nullptr) {
        chain.projTranslator = std::make_unique<codegen::ProjectionTranslator>(projOp);
        chain.projTranslator->setParent(chain.bottom);
        if (chain.sortTranslator) {
            chain.sortTranslator->setChild(chain.projTranslator.get());
        } else {
            chain.resultWriter->setChild(chain.projTranslator.get());
        }
        chain.bottom = chain.projTranslator.get();
    }

    return chain;
}

// ── codegenAllPipelines ──────────────────────────────────────────────────────

std::vector<CodegenResult> PipelineExecutor::codegenAllPipelines(
    const optimizer::OptimizeResult& result) {
    const auto* scanOp = static_cast<const optimizer::PhysicalTableScan*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::TABLE_SCAN));
    const auto* aggOp = static_cast<const optimizer::PhysicalHashAggregate*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::HASH_AGGREGATE));
    const auto* projOp = static_cast<const optimizer::PhysicalProjection*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::PROJECTION));

    std::vector<CodegenResult> pipelines;

    if (aggOp != nullptr) {
        // ── 2-pipeline aggregate ────────────────────────────────────────────
        assert(scanOp != nullptr && "Aggregate query must have a table scan");

        // Pipeline 0: TableScan → HashAggregate(build)
        {
            auto scanTranslator = std::make_unique<codegen::TableScanTranslator>(scanOp);
            hashAggTranslator_ = std::make_unique<codegen::HashAggregateTranslator>(aggOp);
            scanTranslator->setParent(hashAggTranslator_.get());
            hashAggTranslator_->setChild(scanTranslator.get());

            codegen::CodegenContext cg;
            codegen::setActiveContext(&cg);
            auto funcId =
                cg.createFunction("pipeline_build", ir::TypeId::VOID,
                                  {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
            codegen::CompilationContext ctx(cg);
            ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
            ctx.startRow = cg.getArg<codegen::Int64>(1);
            ctx.endRow = cg.getArg<codegen::Int64>(2);
            hashAggTranslator_->produce(ctx);
            cg.emitRetVoid();
            auto program = cg.finalize();
            codegen::setActiveContext(nullptr);

            pipelines.push_back({std::move(program), funcId, "build"});
        }

        // Pipeline 1: HashAggregate(scan) → [Proj →] [Sort →] ResultWriter
        {
            uint32_t numOutputCols =
                projOp != nullptr
                    ? static_cast<uint32_t>(projOp->expressions.size())
                    : static_cast<uint32_t>(aggOp->groupByKeys.size() + aggOp->aggregates.size());

            auto chain = buildOutputChain(result, numOutputCols);
            hashAggTranslator_->setParent(chain.bottom);
            if (chain.projTranslator) {
                chain.projTranslator->setChild(nullptr);
            } else if (chain.sortTranslator) {
                chain.sortTranslator->setChild(nullptr);
            } else {
                chain.resultWriter->setChild(nullptr);
            }

            codegen::CodegenContext cg;
            codegen::setActiveContext(&cg);
            auto funcId =
                cg.createFunction("pipeline_scan", ir::TypeId::VOID,
                                  {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
            codegen::CompilationContext ctx(cg);
            ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
            ctx.startRow = cg.getArg<codegen::Int64>(1);
            ctx.endRow = cg.getArg<codegen::Int64>(2);
            hashAggTranslator_->produceScan(ctx);
            cg.emitRetVoid();
            auto program = cg.finalize();
            codegen::setActiveContext(nullptr);

            pipelines.push_back({std::move(program), funcId, "scan"});
        }
    } else {
        // ── Single pipeline: TableScan → [Proj →] [Sort →] ResultWriter ────
        assert(scanOp != nullptr && "Query must have a table scan");

        uint32_t numOutputCols = projOp != nullptr
                                     ? static_cast<uint32_t>(projOp->expressions.size())
                                     : static_cast<uint32_t>(scanOp->columnIndices.size());

        auto chain = buildOutputChain(result, numOutputCols);

        auto scanTranslator = std::make_unique<codegen::TableScanTranslator>(scanOp);
        scanTranslator->setParent(chain.bottom);
        // Wire the bottom of the chain to the scan.
        if (chain.projTranslator) {
            chain.projTranslator->setChild(scanTranslator.get());
        } else if (chain.sortTranslator) {
            chain.sortTranslator->setChild(scanTranslator.get());
        } else {
            chain.resultWriter->setChild(scanTranslator.get());
        }

        codegen::CodegenContext cg;
        codegen::setActiveContext(&cg);
        auto funcId = cg.createFunction("pipeline_0", ir::TypeId::VOID,
                                        {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
        codegen::CompilationContext ctx(cg);
        ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
        ctx.startRow = cg.getArg<codegen::Int64>(1);
        ctx.endRow = cg.getArg<codegen::Int64>(2);
        chain.resultWriter->produce(ctx);
        cg.emitRetVoid();
        auto program = cg.finalize();
        codegen::setActiveContext(nullptr);

        pipelines.push_back({std::move(program), funcId, "pipeline_0"});
    }

    return pipelines;
}

// ── generateIR / generateASM ─────────────────────────────────────────────────

std::string PipelineExecutor::generateIR(const optimizer::OptimizeResult& result) {
    auto pipelines = codegenAllPipelines(result);
    hashAggTranslator_.reset();  // Explain must not leave side effects.

    std::string irText;
    for (size_t i = 0; i < pipelines.size(); ++i) {
        irText += "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name + " ===\n";
        irText += ir::IRPrinter::print(pipelines[i].program);
        if (i + 1 < pipelines.size()) {
            irText += "\n";
        }
    }
    return irText;
}

std::string PipelineExecutor::generateASM(const optimizer::OptimizeResult& result) {
    auto pipelines = codegenAllPipelines(result);
    hashAggTranslator_.reset();  // Explain must not leave side effects.

    std::string asmText;
    for (size_t i = 0; i < pipelines.size(); ++i) {
#ifdef UMBRA_LLVM_BACKEND
        if (backend_ == Backend::ADAPTIVE) {
            // Adaptive: show both Flying Start and LLVM output.
            std::string fsAsm;
            flying_start::FlyingStartCompiler fsCompiler;
            fsCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, fsAsm);
            asmText += "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name +
                       " (Flying Start) ===\n";
            asmText += fsAsm;
            asmText += "\n";

            std::string llvmAsm;
            llvm_backend::LLVMCompiler llvmCompiler;
            llvmCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, llvmAsm);
            asmText +=
                "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name + " (LLVM O3) ===\n";
            asmText += llvmAsm;
        } else if (backend_ == Backend::LLVM) {
            std::string pipelineAsm;
            llvm_backend::LLVMCompiler llvmCompiler;
            llvmCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, pipelineAsm);
            asmText += "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name + " ===\n";
            asmText += pipelineAsm;
        } else
#endif
        {
            std::string pipelineAsm;
            flying_start::FlyingStartCompiler jitCompiler;
            jitCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, pipelineAsm);
            asmText += "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name + " ===\n";
            asmText += pipelineAsm;
        }
        if (i + 1 < pipelines.size()) {
            asmText += "\n";
        }
    }
    return asmText;
}

// ── Constructor / Public API ─────────────────────────────────────────────────

PipelineExecutor::PipelineExecutor(Backend backend, uint64_t morselSize)
    : backend_(backend), morselSize_(morselSize) {}

void PipelineExecutor::execute(const optimizer::OptimizeResult& result, Table& table,
                               ResultCollector& collector) {
    const auto* aggOp = findOperator(result.root, optimizer::PhysicalNodeKind::HASH_AGGREGATE);

    if (aggOp != nullptr) {
        // 2-pipeline aggregate path.
        if (profiling_) {
            profiles_.resize(2);
        }
        executeBuildPipeline(result, table);
        executeScanPipeline(result, table, collector);
    } else {
        // Single-pipeline path (no aggregate).
        if (profiling_) {
            profiles_.resize(1);
        }
        executeSinglePipeline(result, table, collector);
    }
}

void PipelineExecutor::executeSingle(size_t pipelineIndex, const optimizer::OptimizeResult& result,
                                     Table& table, ResultCollector& collector) {
    const auto* aggOp = findOperator(result.root, optimizer::PhysicalNodeKind::HASH_AGGREGATE);

    if (aggOp != nullptr) {
        // Aggregate query: pipeline 0 = build, pipeline 1 = scan.
        if (pipelineIndex == 0) {
            executeBuildPipeline(result, table);
        } else if (pipelineIndex == 1) {
            if (hashAggTranslator_ == nullptr) {
                executeBuildPipeline(result, table);
            }
            executeScanPipeline(result, table, collector);
        }
    } else {
        // Non-aggregate query: single pipeline.
        if (pipelineIndex == 0) {
            executeSinglePipeline(result, table, collector);
        }
    }
}

codegen::HashTable* PipelineExecutor::hashTable() const {
    if (hashAggTranslator_ != nullptr) {
        return hashAggTranslator_->hashTablePtr();
    }
    return nullptr;
}

// ── Execute pipelines (codegen + compile + run) ──────────────────────────────

void PipelineExecutor::executeBuildPipeline(const optimizer::OptimizeResult& result, Table& table) {
    // Find physical operators.
    const auto* scanOp = static_cast<const optimizer::PhysicalTableScan*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::TABLE_SCAN));
    const auto* aggOp = static_cast<const optimizer::PhysicalHashAggregate*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::HASH_AGGREGATE));
    assert(scanOp != nullptr && "Must have a table scan");
    assert(aggOp != nullptr && "Must have a hash aggregate");

    // Create operator translators.
    auto scanTranslator = std::make_unique<codegen::TableScanTranslator>(scanOp);
    hashAggTranslator_ = std::make_unique<codegen::HashAggregateTranslator>(aggOp);

    // Wire up the produce/consume chain.
    // TableScan -> HashAggregate (build side).
    scanTranslator->setParent(hashAggTranslator_.get());
    hashAggTranslator_->setChild(scanTranslator.get());

    // ── Codegen phase: generate IR via Tidy Tuples ──────────────────────
    auto codegenStart = now();

    codegen::CodegenContext cg;
    codegen::setActiveContext(&cg);

    auto funcId = cg.createFunction("pipeline_build", ir::TypeId::VOID,
                                    {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
    ctx.startRow = cg.getArg<codegen::Int64>(1);
    ctx.endRow = cg.getArg<codegen::Int64>(2);

    hashAggTranslator_->produce(ctx);

    cg.emitRetVoid();

    auto codegenEnd = now();

    // ── Compile phase: finalize IR (+ JIT for Flying Start) ─────────────
    auto compileStart = now();

    auto program = cg.finalize();

    // Set up RuntimeContext.
    table.finalize();
    RuntimeContext rtCtx{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;
    rtCtx.hashTable = hashAggTranslator_->hashTablePtr();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    uint64_t totalItems = table.rowCount;

    // Compile + execute through selected backend.
    // Each compiler must stay alive through execution (it owns JIT'd code).
    void* jitFunc = nullptr;
    flying_start::FlyingStartCompiler jitCompiler;
#ifdef UMBRA_LLVM_BACKEND
    std::unique_ptr<llvm_backend::LLVMCompiler> llvmCompiler;
#endif

    // For ADAPTIVE, compile with FS and start async LLVM.
    // For other backends, compile with the selected backend only.
    // sharedProgram must outlive jitFunc: FS JIT code holds raw pointers to
    // host functions owned by the Program.
    AsyncCompiler asyncLlvm;
    std::shared_ptr<ir::Program> sharedProgram;

    if (backend_ == Backend::ADAPTIVE) {
        // Start async LLVM compile first — gives LLVM a head start while FS compiles.
        // Both backends read the Program concurrently (read-only, no data race).
        sharedProgram = std::make_shared<ir::Program>(std::move(program));
        asyncLlvm.startAsync(sharedProgram, funcId);

        // Flying Start compile (instant, ~0.2ms).
        jitFunc = jitCompiler.compile(*sharedProgram, funcId);
    } else {
        switch (backend_) {
            case Backend::FLYING_START:
                jitFunc = jitCompiler.compile(program, funcId);
                break;
            case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
                llvmCompiler = std::make_unique<llvm_backend::LLVMCompiler>();
                jitFunc = llvmCompiler->compile(program, funcId);
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
                break;
            case Backend::INTERPRETER:
                break;
            case Backend::ADAPTIVE:
                break;  // unreachable
        }
    }

    auto compileEnd = now();

    // ── Execute phase ───────────────────────────────────────────────────
    auto execStart = now();
    uint32_t morselCount = 0;
    uint32_t fsMorselCount = 0;
    uint32_t llvmMorselCount = 0;
    uint32_t switchMorsel = 0;

    if (backend_ == Backend::ADAPTIVE) {
        // Morsel-driven adaptive dispatch: start with FS, switch to LLVM when ready.
        void* activeFunc = jitFunc;
        bool usingLlvm = false;
        uint64_t cursor = 0;

        uint64_t args[3] = {ctxArg, 0, 0};
        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            flying_start::FlyingStartCompiler::execute(activeFunc, std::span<const uint64_t>(args));
            cursor = end;
            ++morselCount;

            if (usingLlvm) {
                ++llvmMorselCount;
            } else {
                ++fsMorselCount;
            }

            // Check if LLVM is ready at morsel boundary.
            if (!usingLlvm && asyncLlvm.isReady()) {
                activeFunc = asyncLlvm.getJitFunc();
                usingLlvm = true;
                switchMorsel = morselCount;  // Switch happens after this morsel.
            }
        }
    } else if (backend_ == Backend::INTERPRETER) {
        std::vector<uint64_t> args = {ctxArg, 0, totalItems};
        interpreter::Interpreter interp;
        interp.execute(program, funcId, std::span<const uint64_t>(args));
    } else {
        // Morsel-driven execution for FS / LLVM (same batch size as adaptive).
        uint64_t cursor = 0;
        uint64_t args[3] = {ctxArg, 0, 0};
        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            if (backend_ == Backend::FLYING_START) {
                flying_start::FlyingStartCompiler::execute(jitFunc,
                                                           std::span<const uint64_t>(args));
            } else {
#ifdef UMBRA_LLVM_BACKEND
                llvm_backend::LLVMCompiler::execute(jitFunc, std::span<const uint64_t>(args));
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            }
            cursor = end;
            ++morselCount;
        }
    }

    auto execEnd = now();

    codegen::setActiveContext(nullptr);

    // Record profiling data.
    if (profiling_) {
        profiles_[0].codegenMs = elapsedMs(codegenStart, codegenEnd);
        profiles_[0].compileMs = elapsedMs(compileStart, compileEnd);
        profiles_[0].executeMs = elapsedMs(execStart, execEnd);
        profiles_[0].totalMorsels = morselCount;
        profiles_[0].fsMorsels = fsMorselCount;
        profiles_[0].llvmMorsels = llvmMorselCount;
        profiles_[0].switchMorsel = switchMorsel;
        if (asyncLlvm.started() && asyncLlvm.isReady()) {
            profiles_[0].llvmCompileMs = asyncLlvm.compileMs();
        }
    }
}

void PipelineExecutor::executeScanPipeline(const optimizer::OptimizeResult& result, Table& table,
                                           ResultCollector& collector) {
    assert(hashAggTranslator_ != nullptr && "Build pipeline must run first");

    // Find physical operators.
    const auto* projOp = static_cast<const optimizer::PhysicalProjection*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::PROJECTION));
    const auto* sortOp = static_cast<const optimizer::PhysicalSort*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::SORT));

    // Determine output schema from projection (or agg keys + agg results).
    std::vector<ResultColumn> schema;
    if (projOp != nullptr) {
        for (size_t i = 0; i < projOp->expressions.size(); ++i) {
            std::string alias =
                i < projOp->aliases.size() ? projOp->aliases[i] : "col_" + std::to_string(i);
            schema.push_back({alias, projOp->expressions[i]->resultType});
        }
    } else {
        const auto* aggOp = hashAggTranslator_->physicalOp();
        for (size_t i = 0; i < aggOp->groupByKeys.size(); ++i) {
            schema.push_back({"group_" + std::to_string(i), aggOp->groupByKeys[i]->resultType});
        }
        for (size_t i = 0; i < aggOp->aggregates.size(); ++i) {
            schema.push_back({"agg_" + std::to_string(i), aggOp->aggregates[i]->resultType});
        }
    }
    auto numOutputCols = static_cast<uint32_t>(schema.size());
    collector.setSchema(std::move(schema));

    // Prepare result collector buffer (upper bound: hash table capacity).
    collector.prepare(hashAggTranslator_->hashTable().capacity());

    // Build output chain: [Sort →] [Proj →] ResultWriter.
    auto chain = buildOutputChain(result, numOutputCols);

    // HashAggregate scan side feeds into the chain.
    hashAggTranslator_->setParent(chain.bottom);
    if (chain.projTranslator) {
        chain.projTranslator->setChild(nullptr);  // Hash agg scan drives produce.
    } else if (chain.sortTranslator) {
        chain.sortTranslator->setChild(nullptr);
    } else {
        chain.resultWriter->setChild(nullptr);
    }

    // ── Codegen phase: generate IR via Tidy Tuples ──────────────────────
    auto codegenStart = now();

    codegen::CodegenContext cg;
    codegen::setActiveContext(&cg);

    auto funcId = cg.createFunction("pipeline_scan", ir::TypeId::VOID,
                                    {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
    ctx.startRow = cg.getArg<codegen::Int64>(1);
    ctx.endRow = cg.getArg<codegen::Int64>(2);

    hashAggTranslator_->produceScan(ctx);

    cg.emitRetVoid();

    auto codegenEnd = now();

    // ── Compile phase: finalize IR (+ JIT for Flying Start) ─────────────
    auto compileStart = now();

    auto program = cg.finalize();

    // Set up RuntimeContext for scan pipeline.
    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;
    rtCtx.hashTable = hashAggTranslator_->hashTablePtr();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    rtCtx.resultBuffer = reinterpret_cast<void*>(collector.buffer());
    rtCtx.resultRowCount = 0;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    auto totalItems = static_cast<uint64_t>(hashAggTranslator_->hashTable().capacity());

    // Compile + execute through selected backend.
    // Each compiler must stay alive through execution (it owns JIT'd code).
    void* jitFunc = nullptr;
    flying_start::FlyingStartCompiler jitCompiler;
#ifdef UMBRA_LLVM_BACKEND
    std::unique_ptr<llvm_backend::LLVMCompiler> llvmCompiler;
#endif

    // For ADAPTIVE, compile with FS and start async LLVM.
    // sharedProgram must outlive jitFunc: FS JIT code holds raw pointers to
    // host functions owned by the Program.
    AsyncCompiler asyncLlvm;
    std::shared_ptr<ir::Program> sharedProgram;

    if (backend_ == Backend::ADAPTIVE) {
        sharedProgram = std::make_shared<ir::Program>(std::move(program));
        asyncLlvm.startAsync(sharedProgram, funcId);
        jitFunc = jitCompiler.compile(*sharedProgram, funcId);
    } else {
        switch (backend_) {
            case Backend::FLYING_START:
                jitFunc = jitCompiler.compile(program, funcId);
                break;
            case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
                llvmCompiler = std::make_unique<llvm_backend::LLVMCompiler>();
                jitFunc = llvmCompiler->compile(program, funcId);
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
                break;
            case Backend::INTERPRETER:
                break;
            case Backend::ADAPTIVE:
                break;  // unreachable
        }
    }

    auto compileEnd = now();

    // ── Execute phase ───────────────────────────────────────────────────
    auto execStart = now();
    uint32_t morselCount = 0;
    uint32_t fsMorselCount = 0;
    uint32_t llvmMorselCount = 0;
    uint32_t switchMorsel = 0;

    if (backend_ == Backend::ADAPTIVE) {
        void* activeFunc = jitFunc;
        bool usingLlvm = false;
        uint64_t cursor = 0;
        uint64_t args[3] = {ctxArg, 0, 0};

        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            flying_start::FlyingStartCompiler::execute(activeFunc, std::span<const uint64_t>(args));
            cursor = end;
            ++morselCount;

            if (usingLlvm) {
                ++llvmMorselCount;
            } else {
                ++fsMorselCount;
            }

            if (!usingLlvm && asyncLlvm.isReady()) {
                activeFunc = asyncLlvm.getJitFunc();
                usingLlvm = true;
                switchMorsel = morselCount;
            }
        }
    } else if (backend_ == Backend::INTERPRETER) {
        std::vector<uint64_t> args = {ctxArg, 0, totalItems};
        interpreter::Interpreter interp;
        interp.execute(program, funcId, std::span<const uint64_t>(args));
    } else {
        uint64_t cursor = 0;
        uint64_t args[3] = {ctxArg, 0, 0};
        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            if (backend_ == Backend::FLYING_START) {
                flying_start::FlyingStartCompiler::execute(jitFunc,
                                                           std::span<const uint64_t>(args));
            } else {
#ifdef UMBRA_LLVM_BACKEND
                llvm_backend::LLVMCompiler::execute(jitFunc, std::span<const uint64_t>(args));
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            }
            cursor = end;
            ++morselCount;
        }
    }

    auto execEnd = now();

    // Materialize results.
    collector.materialize(rtCtx.resultRowCount);

    // Global aggregate (no GROUP BY) must return one row even with no input rows.
    // SQL semantics: COUNT(*)=0, SUM=0 for empty input.
    if (collector.numRows() == 0 && hashAggTranslator_->physicalOp()->groupByKeys.empty()) {
        std::vector<int64_t> defaultRow(numOutputCols, 0);
        collector.addRow(defaultRow);
    }

    // Apply sort if needed (host-side sort on materialized results).
    applyHostSort(sortOp, projOp, nullptr, collector);

    codegen::setActiveContext(nullptr);

    // Record profiling data.
    if (profiling_) {
        profiles_[1].codegenMs = elapsedMs(codegenStart, codegenEnd);
        profiles_[1].compileMs = elapsedMs(compileStart, compileEnd);
        profiles_[1].executeMs = elapsedMs(execStart, execEnd);
        profiles_[1].totalMorsels = morselCount;
        profiles_[1].fsMorsels = fsMorselCount;
        profiles_[1].llvmMorsels = llvmMorselCount;
        profiles_[1].switchMorsel = switchMorsel;
        if (asyncLlvm.started() && asyncLlvm.isReady()) {
            profiles_[1].llvmCompileMs = asyncLlvm.compileMs();
        }
    }
}

// ── Single-pipeline execution (no aggregate) ───────────────────────────────

void PipelineExecutor::executeSinglePipeline(const optimizer::OptimizeResult& result, Table& table,
                                             ResultCollector& collector) {
    // Find physical operators.
    const auto* scanOp = static_cast<const optimizer::PhysicalTableScan*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::TABLE_SCAN));
    const auto* projOp = static_cast<const optimizer::PhysicalProjection*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::PROJECTION));
    const auto* sortOp = static_cast<const optimizer::PhysicalSort*>(
        findOperator(result.root, optimizer::PhysicalNodeKind::SORT));
    assert(scanOp != nullptr && "Query must have a table scan");

    // Determine output schema.
    std::vector<ResultColumn> schema;
    if (projOp != nullptr) {
        for (size_t i = 0; i < projOp->expressions.size(); ++i) {
            std::string alias =
                i < projOp->aliases.size() ? projOp->aliases[i] : "col_" + std::to_string(i);
            schema.push_back({alias, projOp->expressions[i]->resultType});
        }
    } else {
        for (uint32_t colIdx : scanOp->columnIndices) {
            for (const auto& colDef : scanOp->table->columns) {
                if (colDef.columnIndex == colIdx) {
                    schema.push_back({colDef.name, colDef.type});
                    break;
                }
            }
        }
    }
    auto numOutputCols = static_cast<uint32_t>(schema.size());
    collector.setSchema(std::move(schema));

    // Prepare result collector buffer (upper bound: every row passes filter).
    collector.prepare(table.rowCount);

    // Build output chain and wire to table scan.
    auto chain = buildOutputChain(result, numOutputCols);
    auto scanTranslator = std::make_unique<codegen::TableScanTranslator>(scanOp);
    scanTranslator->setParent(chain.bottom);
    if (chain.projTranslator) {
        chain.projTranslator->setChild(scanTranslator.get());
    } else if (chain.sortTranslator) {
        chain.sortTranslator->setChild(scanTranslator.get());
    } else {
        chain.resultWriter->setChild(scanTranslator.get());
    }

    // ── Codegen phase ──────────────────────────────────────────────────
    auto codegenStart = now();

    codegen::CodegenContext cg;
    codegen::setActiveContext(&cg);

    auto funcId = cg.createFunction("pipeline_0", ir::TypeId::VOID,
                                    {ir::TypeId::PTR, ir::TypeId::INT64, ir::TypeId::INT64});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
    ctx.startRow = cg.getArg<codegen::Int64>(1);
    ctx.endRow = cg.getArg<codegen::Int64>(2);

    chain.resultWriter->produce(ctx);

    cg.emitRetVoid();

    auto codegenEnd = now();

    // ── Compile phase ──────────────────────────────────────────────────
    auto compileStart = now();

    auto program = cg.finalize();

    // Set up RuntimeContext.
    table.finalize();
    RuntimeContext rtCtx{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    rtCtx.resultBuffer = reinterpret_cast<void*>(collector.buffer());
    rtCtx.resultRowCount = 0;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    uint64_t totalItems = table.rowCount;

    // Compile through selected backend.
    void* jitFunc = nullptr;
    flying_start::FlyingStartCompiler jitCompiler;
#ifdef UMBRA_LLVM_BACKEND
    std::unique_ptr<llvm_backend::LLVMCompiler> llvmCompiler;
#endif
    // sharedProgram must outlive jitFunc: FS JIT code holds raw pointers to
    // host functions owned by the Program.
    AsyncCompiler asyncLlvm;
    std::shared_ptr<ir::Program> sharedProgram;

    if (backend_ == Backend::ADAPTIVE) {
        sharedProgram = std::make_shared<ir::Program>(std::move(program));
        asyncLlvm.startAsync(sharedProgram, funcId);
        jitFunc = jitCompiler.compile(*sharedProgram, funcId);
    } else {
        switch (backend_) {
            case Backend::FLYING_START:
                jitFunc = jitCompiler.compile(program, funcId);
                break;
            case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
                llvmCompiler = std::make_unique<llvm_backend::LLVMCompiler>();
                jitFunc = llvmCompiler->compile(program, funcId);
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
                break;
            case Backend::INTERPRETER:
                break;
            case Backend::ADAPTIVE:
                break;  // unreachable
        }
    }

    auto compileEnd = now();

    // ── Execute phase ──────────────────────────────────────────────────
    auto execStart = now();
    uint32_t morselCount = 0;
    uint32_t fsMorselCount = 0;
    uint32_t llvmMorselCount = 0;
    uint32_t switchMorsel = 0;

    if (backend_ == Backend::ADAPTIVE) {
        void* activeFunc = jitFunc;
        bool usingLlvm = false;
        uint64_t cursor = 0;
        uint64_t args[3] = {ctxArg, 0, 0};

        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            flying_start::FlyingStartCompiler::execute(activeFunc, std::span<const uint64_t>(args));
            cursor = end;
            ++morselCount;

            if (usingLlvm) {
                ++llvmMorselCount;
            } else {
                ++fsMorselCount;
            }

            if (!usingLlvm && asyncLlvm.isReady()) {
                activeFunc = asyncLlvm.getJitFunc();
                usingLlvm = true;
                switchMorsel = morselCount;
            }
        }
    } else if (backend_ == Backend::INTERPRETER) {
        std::vector<uint64_t> args = {ctxArg, 0, totalItems};
        interpreter::Interpreter interp;
        interp.execute(program, funcId, std::span<const uint64_t>(args));
    } else {
        uint64_t cursor = 0;
        uint64_t args[3] = {ctxArg, 0, 0};
        while (cursor < totalItems) {
            uint64_t end = std::min(cursor + morselSize_, totalItems);
            args[1] = cursor;
            args[2] = end;
            if (backend_ == Backend::FLYING_START) {
                flying_start::FlyingStartCompiler::execute(jitFunc,
                                                           std::span<const uint64_t>(args));
            } else {
#ifdef UMBRA_LLVM_BACKEND
                llvm_backend::LLVMCompiler::execute(jitFunc, std::span<const uint64_t>(args));
#else
                throw std::runtime_error(
                    "LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            }
            cursor = end;
            ++morselCount;
        }
    }

    auto execEnd = now();

    // Materialize results.
    collector.materialize(rtCtx.resultRowCount);

    // Apply sort if needed.
    applyHostSort(sortOp, projOp, scanOp, collector);

    codegen::setActiveContext(nullptr);

    // Record profiling data.
    if (profiling_) {
        profiles_[0].codegenMs = elapsedMs(codegenStart, codegenEnd);
        profiles_[0].compileMs = elapsedMs(compileStart, compileEnd);
        profiles_[0].executeMs = elapsedMs(execStart, execEnd);
        profiles_[0].totalMorsels = morselCount;
        profiles_[0].fsMorsels = fsMorselCount;
        profiles_[0].llvmMorsels = llvmMorselCount;
        profiles_[0].switchMorsel = switchMorsel;
        if (asyncLlvm.started() && asyncLlvm.isReady()) {
            profiles_[0].llvmCompileMs = asyncLlvm.compileMs();
        }
    }
}

// ── Host-side sort ──────────────────────────────────────────────────────────

void PipelineExecutor::applyHostSort(const optimizer::PhysicalSort* sortOp,
                                     const optimizer::PhysicalProjection* projOp,
                                     const optimizer::PhysicalTableScan* scanOp,
                                     ResultCollector& collector) {
    if (sortOp == nullptr) {
        return;
    }

    std::vector<uint32_t> sortCols;
    std::vector<bool> ascending;
    for (const auto& item : sortOp->orderBy) {
        if (item.expr->kind == binder::BoundExprKind::GROUP_REF) {
            const auto* ref = static_cast<const binder::BoundGroupRef*>(item.expr);
            sortCols.push_back(ref->groupIndex);
        } else if (item.expr->kind == binder::BoundExprKind::COLUMN_REF) {
            const auto* colRef = static_cast<const binder::BoundColumnRef*>(item.expr);
            // Map table column index to output position.
            bool found = false;
            if (projOp != nullptr) {
                for (uint32_t i = 0; i < projOp->expressions.size(); ++i) {
                    if (projOp->expressions[i]->kind == binder::BoundExprKind::COLUMN_REF) {
                        const auto* projCol =
                            static_cast<const binder::BoundColumnRef*>(projOp->expressions[i]);
                        if (projCol->columnIndex == colRef->columnIndex) {
                            sortCols.push_back(i);
                            found = true;
                            break;
                        }
                    }
                }
            } else if (scanOp != nullptr) {
                for (uint32_t i = 0; i < scanOp->columnIndices.size(); ++i) {
                    if (scanOp->columnIndices[i] == colRef->columnIndex) {
                        sortCols.push_back(i);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                sortCols.push_back(0);  // Fallback.
            }
        } else {
            sortCols.push_back(0);  // Fallback.
        }
        ascending.push_back(item.ascending);
    }

    // Verify no CHAR(n>1) sort columns — host-side sort compares raw int64_t
    // which would be pointer values, not string content.
    const auto& sch = collector.schema();
    for (auto col : sortCols) {
        if (sch[col].type.id == SQLTypeId::CHAR && sch[col].type.maxLen > 1) {
            throw std::runtime_error("ORDER BY on CHAR(" + std::to_string(sch[col].type.maxLen) +
                                     ") not yet supported");
        }
    }
    collector.sort(sortCols, ascending);
}

// ── Utilities ────────────────────────────────────────────────────────────────

const optimizer::PhysicalOperator* PipelineExecutor::findOperator(
    const optimizer::PhysicalOperator* root, optimizer::PhysicalNodeKind kind) {
    if (root == nullptr) {
        return nullptr;
    }
    if (root->kind == kind) {
        return root;
    }

    // Walk down the tree.
    switch (root->kind) {
        case optimizer::PhysicalNodeKind::SORT:
            return findOperator(static_cast<const optimizer::PhysicalSort*>(root)->child, kind);
        case optimizer::PhysicalNodeKind::PROJECTION:
            return findOperator(static_cast<const optimizer::PhysicalProjection*>(root)->child,
                                kind);
        case optimizer::PhysicalNodeKind::HASH_AGGREGATE:
            return findOperator(static_cast<const optimizer::PhysicalHashAggregate*>(root)->child,
                                kind);
        case optimizer::PhysicalNodeKind::TABLE_SCAN:
            return nullptr;
    }
    return nullptr;
}

}  // namespace umbra::execution
