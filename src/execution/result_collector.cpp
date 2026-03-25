#include "execution/result_collector.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace umbra::execution {

void ResultCollector::setSchema(std::vector<ResultColumn> columns) {
    columns_ = std::move(columns);
}

void ResultCollector::addRow(const std::vector<int64_t>& values) {
    assert(values.size() == columns_.size());
    rows_.push_back({values});
}

void ResultCollector::prepare(size_t maxRows) {
    buffer_.resize(maxRows * columns_.size(), 0);
}

void ResultCollector::materialize(size_t numRows) {
    rows_.clear();
    rows_.reserve(numRows);
    auto numCols = columns_.size();
    for (size_t r = 0; r < numRows; ++r) {
        ResultRow row;
        row.values.resize(numCols);
        for (size_t c = 0; c < numCols; ++c) {
            row.values[c] = buffer_[r * numCols + c];
        }
        rows_.push_back(std::move(row));
    }
}

void ResultCollector::sort(const std::vector<uint32_t>& sortColumns,
                           const std::vector<bool>& ascending) {
    assert(sortColumns.size() == ascending.size());

    std::sort(rows_.begin(), rows_.end(),
              [&sortColumns, &ascending](const ResultRow& rowA, const ResultRow& rowB) {
                  for (size_t i = 0; i < sortColumns.size(); ++i) {
                      auto col = sortColumns[i];
                      if (rowA.values[col] != rowB.values[col]) {
                          if (ascending[i]) {
                              return rowA.values[col] < rowB.values[col];
                          }
                          return rowA.values[col] > rowB.values[col];
                      }
                  }
                  return false;  // equal
              });
}

void ResultCollector::clear() {
    rows_.clear();
    buffer_.clear();
}

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
uint64_t ResultCollector::hostWriteValue(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* collector = reinterpret_cast<ResultCollector*>(args[0]);
    auto rowIndex = args[1];
    auto colIndex = args[2];
    auto value = static_cast<int64_t>(args[3]);
    auto numCols = collector->numColumns();
    collector->buffer()[rowIndex * numCols + colIndex] = value;
    return 0;
}

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
uint64_t ResultCollector::hostGetBuffer(const uint64_t* args, uint8_t /*count*/) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
    auto* collector = reinterpret_cast<ResultCollector*>(args[0]);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<uint64_t>(collector->buffer());
}

}  // namespace umbra::execution
