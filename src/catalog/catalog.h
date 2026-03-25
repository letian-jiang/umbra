#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace umbra {

struct ColumnDef {
    std::string name;
    SQLType type;
    uint32_t columnIndex;  // position in Table.columns
};

struct TableDef {
    std::string name;
    std::vector<ColumnDef> columns;
};

struct Catalog {
    std::unordered_map<std::string, TableDef> tables;

    const TableDef* lookupTable(const std::string& name) const;
    const ColumnDef* lookupColumn(const TableDef& table, const std::string& name) const;
    void addTable(TableDef def);
};

}  // namespace umbra
