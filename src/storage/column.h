#pragma once

#include <cstdint>
#include <variant>
#include <vector>

#include "common/types.h"

namespace umbra {

struct Column {
    // Type-erased storage: holds one of the typed vectors
    std::variant<std::vector<int32_t>,      // INTEGER, DATE
                 std::vector<int64_t>,      // BIGINT, DECIMAL
                 std::vector<StringValue>,  // VARCHAR
                 std::vector<int8_t>        // BOOLEAN
                 >
        storage;

    uint32_t elemSize{0};

    // Create a column with the appropriate storage type for the given SQL type
    static Column create(SQLTypeId typeId);

    // Get raw pointer to the underlying array data
    void* data();
    [[nodiscard]] const void* data() const;

    // Number of rows
    [[nodiscard]] size_t size() const;
};

}  // namespace umbra
