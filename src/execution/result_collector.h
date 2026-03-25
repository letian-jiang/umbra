#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/types.h"

namespace umbra::execution {

/// A single output row stored as raw int64 values.
/// The type information is kept separately in column descriptors.
struct ResultRow {
    std::vector<int64_t> values;
};

/// Column descriptor for result formatting.
struct ResultColumn {
    std::string name;
    SQLType type;
};

/// ResultCollector — collects output rows from pipeline execution.
///
/// Provides a buffer that generated code writes into, then converts
/// the raw values into structured result rows for display/verification.
class ResultCollector {
   public:
    /// Set the output schema (column names and types).
    void setSchema(std::vector<ResultColumn> columns);

    /// Get the output schema.
    [[nodiscard]] const std::vector<ResultColumn>& schema() const { return columns_; }

    /// Get the number of output columns.
    [[nodiscard]] size_t numColumns() const { return columns_.size(); }

    /// Add a result row.
    void addRow(const std::vector<int64_t>& values);

    /// Get all result rows.
    [[nodiscard]] const std::vector<ResultRow>& rows() const { return rows_; }

    /// Get the number of result rows.
    [[nodiscard]] size_t numRows() const { return rows_.size(); }

    /// Get a raw result buffer for generated code to write into.
    /// The buffer holds space for maxRows * numColumns int64_t values.
    /// Generated code writes: buffer[rowIndex * numColumns + colIndex].
    [[nodiscard]] int64_t* buffer() { return buffer_.data(); }

    /// Prepare the buffer for receiving results.
    /// @param maxRows Maximum expected number of rows.
    void prepare(size_t maxRows);

    /// Materialize results from the raw buffer into ResultRow objects.
    /// @param numRows The actual number of rows written.
    void materialize(size_t numRows);

    /// Sort the result rows by the given column indices.
    /// @param sortColumns Column indices to sort by (in priority order).
    /// @param ascending Whether each sort column is ascending.
    void sort(const std::vector<uint32_t>& sortColumns, const std::vector<bool>& ascending);

    /// Clear all collected results.
    void clear();

    /// Host function: write a value to the result buffer.
    /// args: [collector_ptr, row_index, col_index, value]
    static uint64_t hostWriteValue(const uint64_t* args, uint8_t count);

    /// Host function: get the result buffer pointer.
    /// args: [collector_ptr]
    static uint64_t hostGetBuffer(const uint64_t* args, uint8_t count);

   private:
    std::vector<ResultColumn> columns_;
    std::vector<ResultRow> rows_;
    std::vector<int64_t> buffer_;
};

}  // namespace umbra::execution
