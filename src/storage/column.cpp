#include "storage/column.h"

namespace umbra {

Column Column::create(SQLType type) {
    Column col;
    switch (type.id) {
        case SQLTypeId::INTEGER:
        case SQLTypeId::DATE:
            col.storage = std::vector<int32_t>{};
            col.elemSize = sizeof(int32_t);
            break;
        case SQLTypeId::BIGINT:
        case SQLTypeId::DECIMAL:
            col.storage = std::vector<int64_t>{};
            col.elemSize = sizeof(int64_t);
            break;
        case SQLTypeId::CHAR:
            col.storage = std::vector<int8_t>{};
            col.elemSize = type.maxLen;
            break;
        case SQLTypeId::VARCHAR:
            col.storage = std::vector<StringValue>{};
            col.elemSize = sizeof(StringValue);
            break;
        case SQLTypeId::BOOLEAN:
            col.storage = std::vector<int8_t>{};
            col.elemSize = sizeof(int8_t);
            break;
    }
    return col;
}

void* Column::data() {
    return std::visit([](auto& vec) -> void* { return vec.data(); }, storage);
}

const void* Column::data() const {
    return std::visit([](const auto& vec) -> const void* { return vec.data(); }, storage);
}

size_t Column::size() const {
    return std::visit([](const auto& vec) -> size_t { return vec.size(); }, storage);
}

}  // namespace umbra
