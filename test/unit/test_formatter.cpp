#include <catch2/catch_test_macros.hpp>
#include <string>

#include "common/types.h"
#include "execution/result_collector.h"
#include "shell/formatter.h"

using namespace umbra;
using namespace umbra::shell;
using namespace umbra::execution;

TEST_CASE("formatValue - INTEGER", "[formatter]") {
    auto type = SQLType::integer();
    REQUIRE(formatValue(42, type) == "42");
    REQUIRE(formatValue(-1, type) == "-1");
    REQUIRE(formatValue(0, type) == "0");
}

TEST_CASE("formatValue - BIGINT", "[formatter]") {
    auto type = SQLType::bigint();
    REQUIRE(formatValue(1234567890, type) == "1234567890");
    REQUIRE(formatValue(-100, type) == "-100");
}

TEST_CASE("formatValue - DECIMAL", "[formatter]") {
    auto type = SQLType::decimal(15, 2);

    SECTION("Positive value") {
        REQUIRE(formatValue(12345, type) == "123.45");
    }

    SECTION("Negative value") {
        REQUIRE(formatValue(-12345, type) == "-123.45");
    }

    SECTION("Zero") {
        REQUIRE(formatValue(0, type) == "0.00");
    }

    SECTION("Small value with leading zeros") {
        REQUIRE(formatValue(5, type) == "0.05");
    }

    SECTION("Scale 0") {
        auto type0 = SQLType::decimal(10, 0);
        REQUIRE(formatValue(42, type0) == "42");
    }
}

TEST_CASE("formatValue - DATE", "[formatter]") {
    auto type = SQLType::date();

    SECTION("Epoch date 2000-01-01") {
        REQUIRE(formatValue(0, type) == "2000-01-01");
    }

    SECTION("Positive offset") {
        // 2000-01-02
        REQUIRE(formatValue(1, type) == "2000-01-02");
    }

    SECTION("Leap year Feb 29") {
        // 2000 is a leap year. Jan has 31 days, so Feb 29 = day 59.
        REQUIRE(formatValue(59, type) == "2000-02-29");
    }

    SECTION("Negative offset (before epoch)") {
        // 1999-12-31
        REQUIRE(formatValue(-1, type) == "1999-12-31");
    }

    SECTION("Far future date spanning multiple years") {
        // 2002-01-01 = day 731 (2000: 366 days, 2001: 365 days)
        REQUIRE(formatValue(731, type) == "2002-01-01");
    }

    SECTION("Year boundary") {
        // 2000-12-31 = day 365 (2000 is leap, 366 days, so day 365 = Dec 31)
        REQUIRE(formatValue(365, type) == "2000-12-31");
        // 2001-01-01 = day 366
        REQUIRE(formatValue(366, type) == "2001-01-01");
    }
}

TEST_CASE("formatValue - VARCHAR", "[formatter]") {
    auto type = SQLType::varchar(10);

    SECTION("Null pointer") {
        REQUIRE(formatValue(0, type) == "(null)");
    }

    SECTION("Valid string pointer") {
        const char* str = "hello";
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        auto val = static_cast<int64_t>(reinterpret_cast<uintptr_t>(str));
        REQUIRE(formatValue(val, type) == "hello");
    }
}

TEST_CASE("formatValue - BOOLEAN", "[formatter]") {
    auto type = SQLType::boolean();
    REQUIRE(formatValue(1, type) == "true");
    REQUIRE(formatValue(0, type) == "false");
}

TEST_CASE("formatTable - basic table", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"name", SQLType::integer()}, {"value", SQLType::integer()}});
    collector.addRow({1, 100});
    collector.addRow({2, 200});

    std::string result = formatTable(collector);

    // Should contain box-drawing characters.
    REQUIRE(result.find("\xe2\x94\x8c") != std::string::npos);  // ┌
    REQUIRE(result.find("\xe2\x94\x82") != std::string::npos);  // │
    REQUIRE(result.find("\xe2\x94\x94") != std::string::npos);  // └

    // Should contain column headers.
    REQUIRE(result.find("name") != std::string::npos);
    REQUIRE(result.find("value") != std::string::npos);

    // Should contain data values.
    REQUIRE(result.find("1") != std::string::npos);
    REQUIRE(result.find("100") != std::string::npos);
    REQUIRE(result.find("2") != std::string::npos);
    REQUIRE(result.find("200") != std::string::npos);
}

TEST_CASE("formatTable - empty result", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"x", SQLType::integer()}});

    std::string result = formatTable(collector);

    // Should still show headers even with no data.
    REQUIRE(result.find("x") != std::string::npos);
}

TEST_CASE("formatTable - no columns", "[formatter]") {
    ResultCollector collector;
    std::string result = formatTable(collector);
    REQUIRE(result.find("(no columns)") != std::string::npos);
}

TEST_CASE("formatTable - mixed types right/left alignment", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"id", SQLType::integer()}, {"total", SQLType::decimal(15, 2)}});
    collector.addRow({42, 12345});

    std::string result = formatTable(collector);

    // Numbers should be right-aligned, so there should be leading spaces.
    // The exact format depends on column widths, but we can verify the structure.
    REQUIRE(result.find("42") != std::string::npos);
    REQUIRE(result.find("123.45") != std::string::npos);
}

TEST_CASE("formatTsv - basic output", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"a", SQLType::integer()}, {"b", SQLType::integer()}});
    collector.addRow({1, 2});
    collector.addRow({3, 4});

    std::string result = formatTsv(collector);

    REQUIRE(result == "a\tb\n1\t2\n3\t4\n");
}

TEST_CASE("formatTsv - single column", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"x", SQLType::integer()}});
    collector.addRow({42});

    std::string result = formatTsv(collector);
    REQUIRE(result == "x\n42\n");
}

TEST_CASE("formatTsv - decimal values", "[formatter]") {
    ResultCollector collector;
    collector.setSchema({{"val", SQLType::decimal(15, 2)}});
    collector.addRow({12345});

    std::string result = formatTsv(collector);
    REQUIRE(result == "val\n123.45\n");
}
