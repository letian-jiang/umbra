#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"
#include "execution/result_collector.h"

namespace umbra::shell {

/// Format a single result value to string based on its type.
std::string formatValue(int64_t value, SQLType type);

/// Format a result set as a box-drawing table.
/// Numbers are right-aligned, strings/dates are left-aligned.
std::string formatTable(const execution::ResultCollector& collector);

/// Format a result set as tab-separated values (for non-interactive mode).
std::string formatTsv(const execution::ResultCollector& collector);

}  // namespace umbra::shell
