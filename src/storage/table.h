#pragma once

#include <string>
#include <vector>

#include "storage/column.h"

namespace umbra {

struct Table {
    std::string name;
    uint64_t rowCount{0};
    std::vector<Column> columns;

    // String storage: all VARCHAR column data references this buffer
    std::vector<char> stringBuffer;

    // Cached column pointers (built at finalize, stable for Table lifetime)
    std::vector<void*> columnPtrs_;

    // Call after all data has been loaded. Caches column pointers.
    void finalize();

    // Get the column pointer array (for RuntimeContext)
    [[nodiscard]] void* const* columnPtrs() const { return columnPtrs_.data(); }
};

}  // namespace umbra
