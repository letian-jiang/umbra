#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "codegen/operators/hash_aggregate.h"
#include "codegen/operators/operator_translator.h"
#include "codegen/structures/hash_table.h"
#include "execution/result_collector.h"
#include "optimizer/optimizer.h"
#include "storage/runtime_context.h"
#include "storage/table.h"

namespace umbra::execution {

/// PipelineExecutor — compiles and executes query pipelines.
///
/// Takes the optimizer output (physical plan + pipeline decomposition),
/// generates IR for each pipeline using Tidy Tuples operator translators,
/// and executes them through the interpreter backend.
///
/// For Q1:
///   Pipeline 0 (build): TableScan(filter) -> HashAggregate(build)
///   Pipeline 1 (scan):  HashAggregate(scan) -> Projection -> Sort -> ResultWriter
class PipelineExecutor {
   public:
    /// Execute all pipelines for the given optimization result.
    /// @param result Optimization result with physical plan and pipelines.
    /// @param table The table to scan.
    /// @param collector Result collector for output rows.
    void execute(const optimizer::OptimizeResult& result, Table& table, ResultCollector& collector);

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
};

}  // namespace umbra::execution
