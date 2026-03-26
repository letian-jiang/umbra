#pragma once

#include <chrono>
#include <string>

#include "binder/binder.h"
#include "catalog/catalog.h"
#include "execution/pipeline.h"
#include "execution/result_collector.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "storage/table.h"

namespace umbra::explain {

/// EXPLAIN levels — each shows progressively more compilation detail.
enum class ExplainLevel {
    AST,       // Parser output (syntax tree)
    LOGICAL,   // Binder output (bound logical plan)
    PHYSICAL,  // Optimizer output (physical plan)
    PIPELINE,  // Pipeline decomposition
    IR,        // Umbra IR text
    ASM,       // x86-64 disassembly (not implemented in MVP)
    PROFILE,   // Execute with per-pipeline timing
    ALL,       // Print all stages (AST through IR, no execution)
};

/// Parse an EXPLAIN level string. Returns true on success.
bool parseExplainLevel(const std::string& str, ExplainLevel& level);

/// Convert an EXPLAIN level to string.
std::string explainLevelToString(ExplainLevel level);

/// Check if a SQL string starts with EXPLAIN, and extract the level and remaining SQL.
/// Returns true if the input is an EXPLAIN statement.
bool parseExplainPrefix(const std::string& sql, ExplainLevel& level, std::string& remainingSql);

/// Run EXPLAIN at the specified level and return the formatted output.
/// For PROFILE level, also executes the query.
/// @param level The EXPLAIN level.
/// @param sql The SQL string (without EXPLAIN prefix).
/// @param catalog The catalog for name resolution.
/// @param table The table for execution (only needed for PROFILE).
/// @param backend Execution backend for PROFILE (default: interpreter).
/// @return Formatted EXPLAIN output string.
std::string runExplain(ExplainLevel level, const std::string& sql, const Catalog& catalog,
                       Table& table, execution::Backend backend = execution::Backend::INTERPRETER);

}  // namespace umbra::explain
