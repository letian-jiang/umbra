#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/operator_translator.h"
#include "codegen/operators/projection.h"
#include "codegen/operators/result_writer.h"
#include "codegen/operators/sort.h"
#include "codegen/structures/hash_table.h"
#include "execution/result_collector.h"
#include "ir/program.h"
#include "optimizer/optimizer.h"
#include "storage/runtime_context.h"
#include "storage/table.h"

namespace umbra::execution {

/// Execution backend selection.
enum class Backend {
    INTERPRETER,   // Reference interpreter — no compilation overhead, slower execution.
    FLYING_START,  // Flying Start JIT — fast single-pass x86-64 compilation, native execution.
    LLVM,          // LLVM JIT — O3-optimized compilation, best execution performance.
    ADAPTIVE,      // Flying Start + async LLVM — morsel-driven, switch at morsel boundary.
};

/// Default morsel size (rows per morsel).
static constexpr uint64_t DEFAULT_MORSEL_SIZE = 10000;

/// Per-pipeline timing breakdown.
struct PipelineProfile {
    double codegenMs{0.0};   // Tidy Tuples IR generation.
    double compileMs{0.0};   // IR finalization + JIT compilation (Flying Start).
    double executeMs{0.0};   // Pipeline function execution.
    uint32_t totalMorsels{0};  // Total morsel count (0 = non-morsel execution).
};

/// Result of codegen for one pipeline.
struct CodegenResult {
    ir::Program program;
    ir::FunctionId funcId{};
    std::string name;
};

/// PipelineExecutor — compiles and executes query pipelines.
///
/// Takes the optimizer output (physical plan + pipeline decomposition),
/// generates IR for each pipeline using Tidy Tuples operator translators,
/// and executes them through the selected backend (Interpreter or Flying Start).
///
/// For Q1:
///   Pipeline 0 (build): TableScan(filter) -> HashAggregate(build)
///   Pipeline 1 (scan):  HashAggregate(scan) -> Projection -> Sort -> ResultWriter
class PipelineExecutor {
   public:
    /// Construct with the specified backend (default: interpreter).
    explicit PipelineExecutor(Backend backend = Backend::INTERPRETER,
                              uint64_t morselSize = DEFAULT_MORSEL_SIZE);

    /// Enable per-pipeline profiling. Must be called before execute().
    void enableProfiling() { profiling_ = true; }

    /// Per-pipeline timing profiles (populated after execute() if profiling enabled).
    [[nodiscard]] const std::vector<PipelineProfile>& profiles() const { return profiles_; }

    /// Execute all pipelines for the given optimization result.
    /// @param result Optimization result with physical plan and pipelines.
    /// @param table The table to scan.
    /// @param collector Result collector for output rows.
    void execute(const optimizer::OptimizeResult& result, Table& table, ResultCollector& collector);

    /// Generate IR for all pipelines (codegen only, no execution).
    /// Works for any plan shape the optimizer can produce.
    std::vector<CodegenResult> codegenAllPipelines(const optimizer::OptimizeResult& result);

    /// Generate IR text for all pipelines (codegen only, no execution).
    std::string generateIR(const optimizer::OptimizeResult& result);

    /// Generate x86-64 assembly text for all pipelines (codegen + compile, no execution).
    std::string generateASM(const optimizer::OptimizeResult& result);

    /// Execute a single pipeline (for testing).
    /// @param pipelineIndex Which pipeline to execute (0 = build, 1 = scan).
    /// @param result Optimization result.
    /// @param table The table to scan.
    /// @param collector Result collector.
    void executeSingle(size_t pipelineIndex, const optimizer::OptimizeResult& result, Table& table,
                       ResultCollector& collector);

    /// Get the hash table (for inspection/testing after build pipeline).
    [[nodiscard]] codegen::HashTable* hashTable() const;

   private:
    /// Selected execution backend.
    Backend backend_;

    /// Morsel size for adaptive execution.
    uint64_t morselSize_;

    /// Profiling state.
    bool profiling_{false};
    std::vector<PipelineProfile> profiles_;

    /// The hash aggregate translator, shared between pipelines.
    std::unique_ptr<codegen::HashAggregateTranslator> hashAggTranslator_;

    /// Execute pipeline 0 (build side: scan + aggregate).
    void executeBuildPipeline(const optimizer::OptimizeResult& result, Table& table);

    /// Execute pipeline 1 (scan side: agg scan + projection + sort + result).
    void executeScanPipeline(const optimizer::OptimizeResult& result, Table& table,
                             ResultCollector& collector);

    /// Find a specific physical operator by kind in the plan tree.
    static const optimizer::PhysicalOperator* findOperator(const optimizer::PhysicalOperator* root,
                                                           optimizer::PhysicalNodeKind kind);

    /// Build a chain: [Sort →] [Proj →] source, rooted at ResultWriter.
    /// Sets child pointers and returns {resultWriter, bottomParent, all owned translators}.
    /// The caller plugs the source (scan or agg-scan) into bottomParent.
    struct OutputChain {
        std::unique_ptr<codegen::ResultWriterTranslator> resultWriter;
        std::unique_ptr<codegen::SortTranslator> sortTranslator;
        std::unique_ptr<codegen::ProjectionTranslator> projTranslator;
        codegen::OperatorTranslator* bottom;  // Where to plug the source.
    };
    static OutputChain buildOutputChain(const optimizer::OptimizeResult& result, uint32_t numCols);
};

}  // namespace umbra::execution
