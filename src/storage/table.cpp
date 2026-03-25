#include "storage/table.h"

namespace umbra {

void Table::finalize() {
    if (!columns.empty()) {
        rowCount = columns[0].size();
    }
    columnPtrs_.clear();
    columnPtrs_.reserve(columns.size());
    for (auto& col : columns) {
        columnPtrs_.push_back(col.data());
    }
}

}  // namespace umbra
