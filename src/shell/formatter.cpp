#include "shell/formatter.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "common/types.h"
#include "execution/result_collector.h"

namespace umbra::shell {

namespace {

/// Maximum column width before truncation.
constexpr size_t MAX_COL_WIDTH = 50;

/// Check if a type should be right-aligned.
bool isRightAligned(SQLTypeId typeId) {
    return typeId == SQLTypeId::INTEGER || typeId == SQLTypeId::BIGINT ||
           typeId == SQLTypeId::DECIMAL;
}

/// Convert days since epoch (2000-01-01) to YYYY-MM-DD string.
std::string formatDate(int32_t daysSinceEpoch) {
    // Start from 2000-01-01 and add days.
    int32_t y = 2000;
    int32_t m = 1;
    int32_t d = 1;

    int32_t days = daysSinceEpoch;

    // Handle negative days (dates before epoch).
    while (days < 0) {
        --y;
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }

    // Add days year by year.
    while (true) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        int32_t daysInYear = leap ? 366 : 365;
        if (days < daysInYear) {
            break;
        }
        days -= daysInYear;
        ++y;
    }

    // Determine month and day.
    static const std::array<int32_t, 12> DAYS_IN_MONTH = {31, 28, 31, 30, 31, 30,
                                                          31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    for (int32_t mi = 0; mi < 12; ++mi) {
        int32_t dim = DAYS_IN_MONTH[mi];
        if (mi == 1 && leap) {
            dim = 29;
        }
        if (days < dim) {
            m = mi + 1;
            d = days + 1;
            break;
        }
        days -= dim;
    }

    // Format as YYYY-MM-DD.
    std::ostringstream oss;
    oss << y << "-";
    if (m < 10) {
        oss << "0";
    }
    oss << m << "-";
    if (d < 10) {
        oss << "0";
    }
    oss << d;
    return oss.str();
}

/// Format a DECIMAL value with fixed scale.
std::string formatDecimal(int64_t value, uint8_t scale) {
    if (scale == 0) {
        return std::to_string(value);
    }

    bool negative = value < 0;
    uint64_t absVal = negative ? static_cast<uint64_t>(-value) : static_cast<uint64_t>(value);

    uint64_t scaleFactor = 1;
    for (uint8_t i = 0; i < scale; ++i) {
        scaleFactor *= 10;
    }

    uint64_t intPart = absVal / scaleFactor;
    uint64_t fracPart = absVal % scaleFactor;

    std::ostringstream oss;
    if (negative) {
        oss << "-";
    }
    oss << intPart << ".";

    // Pad fractional part with leading zeros.
    std::string fracStr = std::to_string(fracPart);
    while (fracStr.size() < scale) {
        fracStr.insert(fracStr.begin(), '0');
    }
    oss << fracStr;
    return oss.str();
}

/// Pad a string to the given width with the given alignment.
std::string pad(const std::string& str, size_t width, bool rightAlign) {
    if (str.size() >= width) {
        return str;
    }
    size_t padding = width - str.size();
    if (rightAlign) {
        return std::string(padding, ' ') + str;
    }
    return str + std::string(padding, ' ');
}

/// Create a horizontal line using box-drawing characters.
std::string horizontalLine(const std::vector<size_t>& widths, const std::string& left,
                           const std::string& mid, const std::string& right) {
    std::string result = left;
    for (size_t i = 0; i < widths.size(); ++i) {
        for (size_t j = 0; j < widths[i] + 2; ++j) {
            result += "\xe2\x94\x80";  // ─
        }
        if (i < widths.size() - 1) {
            result += mid;
        }
    }
    result += right;
    result += "\n";
    return result;
}

}  // namespace

std::string formatValue(int64_t value, SQLType type) {
    switch (type.id) {
        case SQLTypeId::INTEGER:
            return std::to_string(static_cast<int32_t>(value));
        case SQLTypeId::BIGINT:
            return std::to_string(value);
        case SQLTypeId::DECIMAL:
            return formatDecimal(value, type.scale);
        case SQLTypeId::DATE:
            return formatDate(static_cast<int32_t>(value));
        case SQLTypeId::VARCHAR: {
            // The value is a packed StringValue (offset + length).
            // In the result collector, VARCHAR values are stored as raw int64_t.
            // For display, we just show the raw value.
            // In practice, the result collector stores char pointers for VARCHAR.
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
            auto* ref = reinterpret_cast<const char*>(static_cast<uintptr_t>(value));
            if (ref != nullptr) {
                return {ref};
            }
            return "(null)";
        }
        case SQLTypeId::BOOLEAN:
            return value != 0 ? "true" : "false";
    }
    return std::to_string(value);
}

std::string formatTable(const execution::ResultCollector& collector) {
    const auto& schema = collector.schema();
    const auto& rows = collector.rows();

    if (schema.empty()) {
        return "(no columns)\n";
    }

    size_t numCols = schema.size();

    // Compute column widths.
    std::vector<size_t> widths(numCols);
    for (size_t c = 0; c < numCols; ++c) {
        widths[c] = schema[c].name.size();
    }

    // Format all values and track widths.
    std::vector<std::vector<std::string>> formatted;
    formatted.reserve(rows.size());
    for (const auto& row : rows) {
        std::vector<std::string> formattedRow;
        formattedRow.reserve(numCols);
        for (size_t c = 0; c < numCols; ++c) {
            std::string val = formatValue(row.values[c], schema[c].type);
            widths[c] = std::max(widths[c], val.size());
            formattedRow.push_back(std::move(val));
        }
        formatted.push_back(std::move(formattedRow));
    }

    // Cap column widths.
    for (auto& width : widths) {
        width = std::min(width, MAX_COL_WIDTH);
    }

    std::ostringstream out;

    // Box-drawing characters (UTF-8).
    const std::string TOP_LEFT = "\xe2\x94\x8c";      // NOLINT ┌
    const std::string TOP_RIGHT = "\xe2\x94\x90";     // NOLINT ┐
    const std::string BOTTOM_LEFT = "\xe2\x94\x94";   // NOLINT └
    const std::string BOTTOM_RIGHT = "\xe2\x94\x98";  // NOLINT ┘
    const std::string MID_LEFT = "\xe2\x94\x9c";      // NOLINT ├
    const std::string MID_RIGHT = "\xe2\x94\xa4";     // NOLINT ┤
    const std::string TOP_MID = "\xe2\x94\xac";       // NOLINT ┬
    const std::string BOTTOM_MID = "\xe2\x94\xb4";    // NOLINT ┴
    const std::string CROSS = "\xe2\x94\xbc";         // NOLINT ┼
    const std::string VERTICAL = "\xe2\x94\x82";      // NOLINT │

    // Top border.
    out << horizontalLine(widths, TOP_LEFT, TOP_MID, TOP_RIGHT);

    // Header row.
    out << VERTICAL;
    for (size_t c = 0; c < numCols; ++c) {
        out << " " << pad(schema[c].name, widths[c], false) << " " << VERTICAL;
    }
    out << "\n";

    // Header separator.
    out << horizontalLine(widths, MID_LEFT, CROSS, MID_RIGHT);

    // Data rows.
    for (const auto& formattedRow : formatted) {
        out << VERTICAL;
        for (size_t c = 0; c < numCols; ++c) {
            bool rightAligned = isRightAligned(schema[c].type.id);
            out << " " << pad(formattedRow[c], widths[c], rightAligned) << " " << VERTICAL;
        }
        out << "\n";
    }

    // Bottom border.
    out << horizontalLine(widths, BOTTOM_LEFT, BOTTOM_MID, BOTTOM_RIGHT);

    return out.str();
}

std::string formatTsv(const execution::ResultCollector& collector) {
    const auto& schema = collector.schema();
    const auto& rows = collector.rows();

    std::ostringstream out;

    // Header.
    for (size_t c = 0; c < schema.size(); ++c) {
        if (c > 0) {
            out << "\t";
        }
        out << schema[c].name;
    }
    out << "\n";

    // Data rows.
    for (const auto& row : rows) {
        for (size_t c = 0; c < schema.size(); ++c) {
            if (c > 0) {
                out << "\t";
            }
            out << formatValue(row.values[c], schema[c].type);
        }
        out << "\n";
    }

    return out.str();
}

}  // namespace umbra::shell
