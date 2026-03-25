#pragma once

#include <cstdint>
#include <string>

#include "catalog/catalog.h"
#include "storage/table.h"

namespace umbra {

// Load a .tbl file into a Table based on the schema in TableDef
Table loadTblFile(const std::string& filePath, const TableDef& schema);

// Get the TPC-H lineitem table definition
TableDef lineitemTableDef();

// Parsing helpers
int64_t parseDecimal(const std::string& str, uint8_t scale);
int32_t parseDate(const std::string& str);  // returns days since epoch (2000-01-01)

}  // namespace umbra
