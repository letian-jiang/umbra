#include "storage/tbl_loader.h"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace umbra {

int64_t parseDecimal(const std::string& str, uint8_t scale) {
    bool negative = false;
    size_t start = 0;
    if (!str.empty() && str[0] == '-') {
        negative = true;
        start = 1;
    }

    auto dotPos = str.find('.');
    int64_t intPart = 0;
    int64_t fracPart = 0;

    if (dotPos == std::string::npos) {
        // No decimal point — parse entire string as integer part
        intPart = std::stoll(str.substr(start));
    } else {
        if (dotPos > start) {
            intPart = std::stoll(str.substr(start, dotPos - start));
        }
        std::string fracStr = str.substr(dotPos + 1);
        // Pad or truncate fractional part to match scale
        if (fracStr.size() < scale) {
            fracStr.append(scale - fracStr.size(), '0');
        } else if (fracStr.size() > scale) {
            fracStr = fracStr.substr(0, scale);
        }
        if (!fracStr.empty()) {
            fracPart = std::stoll(fracStr);
        }
    }

    // Compute scale multiplier
    int64_t multiplier = 1;
    for (uint8_t i = 0; i < scale; ++i) {
        multiplier *= 10;
    }

    int64_t result = intPart * multiplier + fracPart;
    if (negative) {
        result = -result;
    }
    return result;
}

// Count days from 2000-01-01 to year-month-day
// Positive if date is after epoch, negative if before
int32_t parseDate(const std::string& str) {
    // Expected format: YYYY-MM-DD
    int year = std::stoi(str.substr(0, 4));
    int month = std::stoi(str.substr(5, 2));
    int day = std::stoi(str.substr(8, 2));

    // Calculate total days from a reference point (year 0) for both dates
    // then subtract. We'll use a simpler approach: accumulate days between
    // epoch and target date.

    int epochYear = DATE_EPOCH_YEAR;
    int epochMonth = DATE_EPOCH_MONTH;
    int epochDay = DATE_EPOCH_DAY;

    // Direction: if target > epoch, count forward; else count backward
    // We'll compute days-from-civil for both and subtract.

    // days from year 0 to (year, month, day) using a standard algorithm
    auto daysSinceRef = [](int y, int m, int d) -> int64_t {
        // Adjust so March is month 0 (simplifies leap year handling)
        if (m <= 2) {
            y--;
            m += 9;
        } else {
            m -= 3;
        }
        // Era-based calculation (from Howard Hinnant's date algorithms)
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        int64_t yoe = y - era * 400;                          // year of era [0, 399]
        int64_t doy = (153 * m + 2) / 5 + d - 1;              // day of year [0, 365]
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // day of era [0, 146096]
        return era * 146097 + doe - 719468;                   // days since 1970-01-01 (Unix epoch)
    };

    auto targetDays = daysSinceRef(year, month, day);
    auto epochDays = daysSinceRef(epochYear, epochMonth, epochDay);

    return static_cast<int32_t>(targetDays - epochDays);
}

TableDef lineitemTableDef() {
    TableDef def;
    def.name = "lineitem";
    def.columns = {
        {"l_orderkey", SQLType::integer(), 0},      {"l_partkey", SQLType::integer(), 1},
        {"l_suppkey", SQLType::integer(), 2},       {"l_linenumber", SQLType::integer(), 3},
        {"l_quantity", SQLType::decimal(15, 2), 4}, {"l_extendedprice", SQLType::decimal(15, 2), 5},
        {"l_discount", SQLType::decimal(15, 2), 6}, {"l_tax", SQLType::decimal(15, 2), 7},
        {"l_returnflag", SQLType::varchar(1), 8},   {"l_linestatus", SQLType::varchar(1), 9},
        {"l_shipdate", SQLType::date(), 10},        {"l_commitdate", SQLType::date(), 11},
        {"l_receiptdate", SQLType::date(), 12},     {"l_shipinstruct", SQLType::varchar(25), 13},
        {"l_shipmode", SQLType::varchar(10), 14},   {"l_comment", SQLType::varchar(44), 15},
    };
    return def;
}

// Split a line by delimiter, returning fields
static std::vector<std::string> splitLine(const std::string& line, char delim) {
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, delim)) {
        fields.push_back(field);
    }
    return fields;
}

Table loadTblFile(const std::string& filePath, const TableDef& schema) {
    Table table;
    table.name = schema.name;

    // Create columns based on schema
    for (const auto& colDef : schema.columns) {
        table.columns.push_back(Column::create(colDef.type.id));
    }

    std::ifstream file(filePath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filePath);
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        auto fields = splitLine(line, '|');
        // dbgen lines end with '|', so the last element is empty — skip it
        if (!fields.empty() && fields.back().empty()) {
            fields.pop_back();
        }

        if (fields.size() != schema.columns.size()) {
            throw std::runtime_error("Field count mismatch: expected " +
                                     std::to_string(schema.columns.size()) + ", got " +
                                     std::to_string(fields.size()));
        }

        for (size_t i = 0; i < fields.size(); ++i) {
            const auto& colDef = schema.columns[i];
            auto& col = table.columns[i];
            const auto& field = fields[i];

            switch (colDef.type.id) {
                case SQLTypeId::INTEGER: {
                    auto& vec = std::get<std::vector<int32_t>>(col.storage);
                    vec.push_back(std::stoi(field));
                    break;
                }
                case SQLTypeId::BIGINT: {
                    auto& vec = std::get<std::vector<int64_t>>(col.storage);
                    vec.push_back(std::stoll(field));
                    break;
                }
                case SQLTypeId::DECIMAL: {
                    auto& vec = std::get<std::vector<int64_t>>(col.storage);
                    vec.push_back(parseDecimal(field, colDef.type.scale));
                    break;
                }
                case SQLTypeId::DATE: {
                    auto& vec = std::get<std::vector<int32_t>>(col.storage);
                    vec.push_back(parseDate(field));
                    break;
                }
                case SQLTypeId::VARCHAR: {
                    auto& vec = std::get<std::vector<StringValue>>(col.storage);
                    StringValue sv;
                    sv.offset = static_cast<uint32_t>(table.stringBuffer.size());
                    sv.len = static_cast<uint32_t>(field.size());
                    table.stringBuffer.insert(table.stringBuffer.end(), field.begin(), field.end());
                    vec.push_back(sv);
                    break;
                }
                case SQLTypeId::BOOLEAN: {
                    auto& vec = std::get<std::vector<int8_t>>(col.storage);
                    vec.push_back(field == "1" || field == "true" || field == "t" ? 1 : 0);
                    break;
                }
            }
        }
    }

    table.finalize();
    return table;
}

}  // namespace umbra
