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
#include "codegen/api/codegen_context.h"
#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/projection.h"
#include "codegen/operators/result_writer.h"
#include "codegen/operators/sort.h"
#include "codegen/operators/table_scan.h"
#include "codegen/sql_values/sql_value.h"
#include "ir/opcode.h"
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
    RuntimeContext rtCtx;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    rtCtx.columnPtrs = const_cast<void**>(table.columnPtrs());
    rtCtx.rowCount = table.rowCount;
    rtCtx.hashTable = hashAggTranslator_->hashTablePtr();

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ctxArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&rtCtx));
    std::vector<uint64_t> args = {ctxArg};

    // Compile + execute through selected backend.
    // The FlyingStartCompiler must stay alive through execution (it owns JIT'd code).
    flying_start::FlyingStartCompiler jitCompiler;
    void* jitFunc = nullptr;

    if (backend_ == Backend::FLYING_START) {
        jitFunc = jitCompiler.compile(program, funcId);
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
        case Backend::FLYING_START: {
            flying_start::FlyingStartCompiler::execute(jitFunc, std::span<const uint64_t>(args));
            break;
        }
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

    // Create operator translators for scan pipeline.
    // Chain: HashAggregate(scan) -> Projection -> Sort -> ResultWriter.
    auto resultWriter = std::make_unique<codegen::ResultWriterTranslator>(numOutputCols);

    // Build the chain from bottom (result writer) to top.
    codegen::OperatorTranslator* currentParent = resultWriter.get();

    std::unique_ptr<codegen::SortTranslator> sortTranslator;
    if (sortOp != nullptr) {
        sortTranslator = std::make_unique<codegen::SortTranslator>(sortOp);
        sortTranslator->setParent(currentParent);
        resultWriter->setChild(sortTranslator.get());
        currentParent = sortTranslator.get();
    } else {
        resultWriter->setChild(nullptr);  // Will be set below.
    }

    std::unique_ptr<codegen::ProjectionTranslator> projTranslator;
    if (projOp != nullptr) {
        projTranslator = std::make_unique<codegen::ProjectionTranslator>(projOp);
        projTranslator->setParent(currentParent);
        if (sortTranslator) {
            sortTranslator->setChild(projTranslator.get());
        } else {
            resultWriter->setChild(projTranslator.get());
        }
        currentParent = projTranslator.get();
    }

    // HashAggregate scan side feeds into the chain.
    hashAggTranslator_->setParent(currentParent);
    if (projTranslator) {
        projTranslator->setChild(nullptr);  // Hash agg scan drives produce.
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
    // The FlyingStartCompiler must stay alive through execution (it owns JIT'd code).
    flying_start::FlyingStartCompiler jitCompiler;
    void* jitFunc = nullptr;

    if (backend_ == Backend::FLYING_START) {
        jitFunc = jitCompiler.compile(program, funcId);
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
        case Backend::FLYING_START: {
            flying_start::FlyingStartCompiler::execute(jitFunc, std::span<const uint64_t>(args));
            break;
        }
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
