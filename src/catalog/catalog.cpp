#include "catalog/catalog.h"

#include <utility>

namespace umbra {

const TableDef* Catalog::lookupTable(const std::string& name) const {
    auto it = tables.find(name);
    if (it == tables.end()) {
        return nullptr;
    }
    return &it->second;
}

const ColumnDef* Catalog::lookupColumn(const TableDef& table, const std::string& name) const {
    for (const auto& col : table.columns) {
        if (col.name == name) {
            return &col;
        }
    }
    return nullptr;
}

void Catalog::addTable(TableDef def) {
    auto name = def.name;
    tables.emplace(std::move(name), std::move(def));
}

}  // namespace umbra
