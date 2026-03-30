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
            auto funcId = cg.createFunction("pipeline_build", ir::TypeId::VOID, {ir::TypeId::PTR});
            codegen::CompilationContext ctx(cg);
            ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
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
            auto funcId = cg.createFunction("pipeline_scan", ir::TypeId::VOID, {ir::TypeId::PTR});
            codegen::CompilationContext ctx(cg);
            ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
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
        auto funcId = cg.createFunction("pipeline_0", ir::TypeId::VOID, {ir::TypeId::PTR});
        codegen::CompilationContext ctx(cg);
        ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);
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
        std::string pipelineAsm;
#ifdef UMBRA_LLVM_BACKEND
        if (backend_ == Backend::LLVM) {
            llvm_backend::LLVMCompiler llvmCompiler;
            llvmCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, pipelineAsm);
        } else
#endif
        {
            flying_start::FlyingStartCompiler jitCompiler;
            jitCompiler.compileWithLog(pipelines[i].program, pipelines[i].funcId, pipelineAsm);
        }

        asmText += "=== Pipeline " + std::to_string(i) + ": " + pipelines[i].name + " ===\n";
        asmText += pipelineAsm;
        if (i + 1 < pipelines.size()) {
            asmText += "\n";
        }
    }
    return asmText;
}

// ── Constructor / Public API ─────────────────────────────────────────────────

PipelineExecutor::PipelineExecutor(Backend backend) : backend_(backend) {}

void PipelineExecutor::execute(const optimizer::OptimizeResult& result, Table& table,
                               ResultCollector& collector) {
    assert(result.pipelines.size() >= 2 && "Q1 must have at least 2 pipelines");

    if (profiling_) {
        profiles_.resize(2);
    }

    // Pipeline 0: build side
    executeBuildPipeline(result, table);

    // Pipeline 1: scan side
    executeScanPipeline(result, table, collector);
}

void PipelineExecutor::executeSingle(size_t pipelineIndex, const optimizer::OptimizeResult& result,
                                     Table& table, ResultCollector& collector) {
    if (pipelineIndex == 0) {
        executeBuildPipeline(result, table);
    } else if (pipelineIndex == 1) {
        // Ensure build pipeline has run first.
        if (hashAggTranslator_ == nullptr) {
            executeBuildPipeline(result, table);
        }
        executeScanPipeline(result, table, collector);
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

    auto funcId = cg.createFunction("pipeline_build", ir::TypeId::VOID, {ir::TypeId::PTR});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);

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
    std::vector<uint64_t> args = {ctxArg};

    // Compile + execute through selected backend.
    // Compile + execute through selected backend.
    // Each compiler must stay alive through execution (it owns JIT'd code).
    void* jitFunc = nullptr;
    flying_start::FlyingStartCompiler jitCompiler;
#ifdef UMBRA_LLVM_BACKEND
    std::unique_ptr<llvm_backend::LLVMCompiler> llvmCompiler;
#endif

    switch (backend_) {
        case Backend::FLYING_START:
            jitFunc = jitCompiler.compile(program, funcId);
            break;
        case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
            llvmCompiler = std::make_unique<llvm_backend::LLVMCompiler>();
            jitFunc = llvmCompiler->compile(program, funcId);
#else
            throw std::runtime_error("LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            break;
        case Backend::INTERPRETER:
            break;
    }

    auto compileEnd = now();

    // ── Execute phase ───────────────────────────────────────────────────
    auto execStart = now();

    switch (backend_) {
        case Backend::INTERPRETER: {
            interpreter::Interpreter interp;
            interp.execute(program, funcId, std::span<const uint64_t>(args));
            break;
        }
        case Backend::FLYING_START:
            flying_start::FlyingStartCompiler::execute(jitFunc, std::span<const uint64_t>(args));
            break;
        case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
            llvm_backend::LLVMCompiler::execute(jitFunc, std::span<const uint64_t>(args));
#else
            throw std::runtime_error("LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            break;
    }

    auto execEnd = now();

    codegen::setActiveContext(nullptr);

    // Record profiling data.
    if (profiling_) {
        profiles_[0].codegenMs = elapsedMs(codegenStart, codegenEnd);
        profiles_[0].compileMs = elapsedMs(compileStart, compileEnd);
        profiles_[0].executeMs = elapsedMs(execStart, execEnd);
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

    // Prepare result collector buffer.
    collector.prepare(1024);  // Max 1024 output rows (plenty for Q1's ~4 rows).

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

    auto funcId = cg.createFunction("pipeline_scan", ir::TypeId::VOID, {ir::TypeId::PTR});
    codegen::CompilationContext ctx(cg);
    ctx.runtimeCtx = cg.getArg<codegen::Ptr<codegen::Int8>>(0);

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
    std::vector<uint64_t> args = {ctxArg};

    // Compile + execute through selected backend.
    // Each compiler must stay alive through execution (it owns JIT'd code).
    void* jitFunc = nullptr;
    flying_start::FlyingStartCompiler jitCompiler;
#ifdef UMBRA_LLVM_BACKEND
    std::unique_ptr<llvm_backend::LLVMCompiler> llvmCompiler;
#endif

    switch (backend_) {
        case Backend::FLYING_START:
            jitFunc = jitCompiler.compile(program, funcId);
            break;
        case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
            llvmCompiler = std::make_unique<llvm_backend::LLVMCompiler>();
            jitFunc = llvmCompiler->compile(program, funcId);
#else
            throw std::runtime_error("LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            break;
        case Backend::INTERPRETER:
            break;
    }

    auto compileEnd = now();

    // ── Execute phase ───────────────────────────────────────────────────
    auto execStart = now();

    switch (backend_) {
        case Backend::INTERPRETER: {
            interpreter::Interpreter interp;
            interp.execute(program, funcId, std::span<const uint64_t>(args));
            break;
        }
        case Backend::FLYING_START:
            flying_start::FlyingStartCompiler::execute(jitFunc, std::span<const uint64_t>(args));
            break;
        case Backend::LLVM:
#ifdef UMBRA_LLVM_BACKEND
            llvm_backend::LLVMCompiler::execute(jitFunc, std::span<const uint64_t>(args));
#else
            throw std::runtime_error("LLVM backend not compiled. Rebuild with -DLLVM_BACKEND=ON");
#endif
            break;
    }

    auto execEnd = now();

    // Materialize results.
    collector.materialize(rtCtx.resultRowCount);

    // Apply sort if needed (host-side sort on materialized results).
    if (sortOp != nullptr) {
        std::vector<uint32_t> sortCols;
        std::vector<bool> ascending;
        for (const auto& item : sortOp->orderBy) {
            // Find the column index in the output.
            // Sort expressions reference group-by keys via BoundGroupRef.
            if (item.expr->kind == binder::BoundExprKind::GROUP_REF) {
                const auto* ref = static_cast<const binder::BoundGroupRef*>(item.expr);
                sortCols.push_back(ref->groupIndex);
            } else {
                // Default: sort by column 0.
                sortCols.push_back(0);
            }
            ascending.push_back(item.ascending);
        }
        // Verify no CHAR(n>1) sort columns — host-side sort compares raw int64_t
        // which would be pointer values, not string content.
        const auto& sch = collector.schema();
        for (auto col : sortCols) {
            if (sch[col].type.id == SQLTypeId::CHAR && sch[col].type.maxLen > 1) {
                throw std::runtime_error("ORDER BY on CHAR(" +
                                         std::to_string(sch[col].type.maxLen) +
                                         ") not yet supported");
            }
        }
        collector.sort(sortCols, ascending);
    }

    codegen::setActiveContext(nullptr);

    // Record profiling data.
    if (profiling_) {
        profiles_[1].codegenMs = elapsedMs(codegenStart, codegenEnd);
        profiles_[1].compileMs = elapsedMs(compileStart, compileEnd);
        profiles_[1].executeMs = elapsedMs(execStart, execEnd);
    }
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
