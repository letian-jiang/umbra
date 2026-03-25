#include <catch2/catch_test_macros.hpp>

#include "common/types.h"

TEST_CASE("Type system basics", "[types]") {
    using namespace umbra;

    SECTION("SQLType factory methods") {
        auto intType = SQLType::integer();
        REQUIRE(intType.id == SQLTypeId::INTEGER);

        auto decType = SQLType::decimal(15, 2);
        REQUIRE(decType.id == SQLTypeId::DECIMAL);
        REQUIRE(decType.precision == 15);
        REQUIRE(decType.scale == 2);

        auto varcharType = SQLType::varchar(100);
        REQUIRE(varcharType.id == SQLTypeId::VARCHAR);
        REQUIRE(varcharType.maxLen == 100);
    }

    SECTION("Type sizes") {
        REQUIRE(typeSize(SQLTypeId::INTEGER) == 4);
        REQUIRE(typeSize(SQLTypeId::BIGINT) == 8);
        REQUIRE(typeSize(SQLTypeId::DATE) == 4);
        REQUIRE(typeSize(SQLTypeId::DECIMAL) == 8);
        REQUIRE(typeSize(SQLTypeId::VARCHAR) == sizeof(StringValue));
        REQUIRE(typeSize(SQLTypeId::BOOLEAN) == 1);
    }

    SECTION("StringRef") {
        const char* str = "hello";
        StringRef ref{str, 5};
        REQUIRE(ref.view() == "hello");
        REQUIRE(ref.toString() == "hello");
    }
}
