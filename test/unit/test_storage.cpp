#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "storage/column.h"
#include "storage/runtime_context.h"
#include "storage/table.h"
#include "storage/tbl_loader.h"

using namespace umbra;

// ── Column tests ─────────────────────────────────────────────────────────────

TEST_CASE("Column create and push", "[storage]") {
    SECTION("INTEGER column") {
        auto col = Column::create(SQLTypeId::INTEGER);
        REQUIRE(col.elemSize == 4);
        REQUIRE(col.size() == 0);

        auto& vec = std::get<std::vector<int32_t>>(col.storage);
        vec.push_back(42);
        vec.push_back(-7);
        REQUIRE(col.size() == 2);

        auto* ptr = static_cast<int32_t*>(col.data());
        REQUIRE(ptr[0] == 42);
        REQUIRE(ptr[1] == -7);
    }

    SECTION("BIGINT column") {
        auto col = Column::create(SQLTypeId::BIGINT);
        REQUIRE(col.elemSize == 8);

        auto& vec = std::get<std::vector<int64_t>>(col.storage);
        vec.push_back(1234567890123LL);
        REQUIRE(col.size() == 1);

        auto* ptr = static_cast<int64_t*>(col.data());
        REQUIRE(ptr[0] == 1234567890123LL);
    }

    SECTION("DECIMAL column") {
        auto col = Column::create(SQLTypeId::DECIMAL);
        REQUIRE(col.elemSize == 8);

        auto& vec = std::get<std::vector<int64_t>>(col.storage);
        vec.push_back(2116823);  // 21168.23 scaled by 100
        REQUIRE(col.size() == 1);

        auto* ptr = static_cast<int64_t*>(col.data());
        REQUIRE(ptr[0] == 2116823);
    }

    SECTION("DATE column") {
        auto col = Column::create(SQLTypeId::DATE);
        REQUIRE(col.elemSize == 4);

        auto& vec = std::get<std::vector<int32_t>>(col.storage);
        vec.push_back(0);   // 2000-01-01
        vec.push_back(-1);  // 1999-12-31
        REQUIRE(col.size() == 2);
    }

    SECTION("VARCHAR column") {
        auto col = Column::create(SQLTypeId::VARCHAR);
        REQUIRE(col.elemSize == 8);

        auto& vec = std::get<std::vector<StringValue>>(col.storage);
        vec.push_back(StringValue{0, 5});
        vec.push_back(StringValue{5, 3});
        REQUIRE(col.size() == 2);

        auto* ptr = static_cast<StringValue*>(col.data());
        REQUIRE(ptr[0].offset == 0);
        REQUIRE(ptr[0].len == 5);
        REQUIRE(ptr[1].offset == 5);
        REQUIRE(ptr[1].len == 3);
    }

    SECTION("BOOLEAN column") {
        auto col = Column::create(SQLTypeId::BOOLEAN);
        REQUIRE(col.elemSize == 1);

        auto& vec = std::get<std::vector<int8_t>>(col.storage);
        vec.push_back(1);
        vec.push_back(0);
        REQUIRE(col.size() == 2);

        auto* ptr = static_cast<int8_t*>(col.data());
        REQUIRE(ptr[0] == 1);
        REQUIRE(ptr[1] == 0);
    }

    SECTION("const data access") {
        auto col = Column::create(SQLTypeId::INTEGER);
        auto& vec = std::get<std::vector<int32_t>>(col.storage);
        vec.push_back(99);

        const auto& constCol = col;
        const auto* ptr = static_cast<const int32_t*>(constCol.data());
        REQUIRE(ptr[0] == 99);
    }
}

// ── Table tests ──────────────────────────────────────────────────────────────

TEST_CASE("Table construction and finalize", "[storage]") {
    Table table;
    table.name = "test_table";

    // Add two columns: INTEGER and VARCHAR
    table.columns.push_back(Column::create(SQLTypeId::INTEGER));
    table.columns.push_back(Column::create(SQLTypeId::VARCHAR));

    // Push data
    auto& intVec = std::get<std::vector<int32_t>>(table.columns[0].storage);
    intVec.push_back(1);
    intVec.push_back(2);
    intVec.push_back(3);

    auto& strVec = std::get<std::vector<StringValue>>(table.columns[1].storage);
    std::string hello = "hello";
    std::string world = "world";
    std::string test = "test";

    // Simulate adding strings to stringBuffer
    StringValue sv1;
    sv1.offset = static_cast<uint32_t>(table.stringBuffer.size());
    sv1.len = static_cast<uint32_t>(hello.size());
    table.stringBuffer.insert(table.stringBuffer.end(), hello.begin(), hello.end());
    strVec.push_back(sv1);

    StringValue sv2;
    sv2.offset = static_cast<uint32_t>(table.stringBuffer.size());
    sv2.len = static_cast<uint32_t>(world.size());
    table.stringBuffer.insert(table.stringBuffer.end(), world.begin(), world.end());
    strVec.push_back(sv2);

    StringValue sv3;
    sv3.offset = static_cast<uint32_t>(table.stringBuffer.size());
    sv3.len = static_cast<uint32_t>(test.size());
    table.stringBuffer.insert(table.stringBuffer.end(), test.begin(), test.end());
    strVec.push_back(sv3);

    // Finalize
    table.finalize();

    REQUIRE(table.rowCount == 3);
    REQUIRE(table.columnPtrs_.size() == 2);

    // Verify column pointers are correct
    REQUIRE(table.columnPtrs()[0] == table.columns[0].data());
    REQUIRE(table.columnPtrs()[1] == table.columns[1].data());

    // Verify data through pointers
    auto* intPtr = static_cast<int32_t*>(table.columnPtrs()[0]);
    REQUIRE(intPtr[0] == 1);
    REQUIRE(intPtr[1] == 2);
    REQUIRE(intPtr[2] == 3);

    // Verify string data
    auto* svPtr = static_cast<StringValue*>(table.columnPtrs()[1]);
    std::string_view s1(table.stringBuffer.data() + svPtr[0].offset, svPtr[0].len);
    std::string_view s2(table.stringBuffer.data() + svPtr[1].offset, svPtr[1].len);
    std::string_view s3(table.stringBuffer.data() + svPtr[2].offset, svPtr[2].len);
    REQUIRE(s1 == "hello");
    REQUIRE(s2 == "world");
    REQUIRE(s3 == "test");
}

TEST_CASE("Table finalize with empty columns", "[storage]") {
    Table table;
    table.name = "empty";
    table.finalize();
    REQUIRE(table.rowCount == 0);
    REQUIRE(table.columnPtrs_.empty());
}

// ── Catalog tests ────────────────────────────────────────────────────────────

TEST_CASE("Catalog operations", "[storage]") {
    Catalog catalog;

    SECTION("Add and lookup table") {
        TableDef def;
        def.name = "orders";
        def.columns = {
            {"o_orderkey", SQLType::integer(), 0},
            {"o_custkey", SQLType::integer(), 1},
            {"o_orderstatus", SQLType::varchar(1), 2},
        };

        catalog.addTable(def);

        const auto* found = catalog.lookupTable("orders");
        REQUIRE(found != nullptr);
        REQUIRE(found->name == "orders");
        REQUIRE(found->columns.size() == 3);
    }

    SECTION("Lookup non-existent table") {
        const auto* found = catalog.lookupTable("nonexistent");
        REQUIRE(found == nullptr);
    }

    SECTION("Lookup column") {
        TableDef def;
        def.name = "test";
        def.columns = {
            {"col_a", SQLType::integer(), 0},
            {"col_b", SQLType::varchar(10), 1},
        };
        catalog.addTable(def);

        const auto* table = catalog.lookupTable("test");
        REQUIRE(table != nullptr);

        const auto* colA = catalog.lookupColumn(*table, "col_a");
        REQUIRE(colA != nullptr);
        REQUIRE(colA->name == "col_a");
        REQUIRE(colA->type.id == SQLTypeId::INTEGER);
        REQUIRE(colA->columnIndex == 0);

        const auto* colB = catalog.lookupColumn(*table, "col_b");
        REQUIRE(colB != nullptr);
        REQUIRE(colB->name == "col_b");
        REQUIRE(colB->type.id == SQLTypeId::VARCHAR);
        REQUIRE(colB->columnIndex == 1);

        const auto* colC = catalog.lookupColumn(*table, "col_c");
        REQUIRE(colC == nullptr);
    }
}

// ── parseDecimal tests ───────────────────────────────────────────────────────

TEST_CASE("parseDecimal", "[storage]") {
    REQUIRE(parseDecimal("21168.23", 2) == 2116823);
    REQUIRE(parseDecimal("0.04", 2) == 4);
    REQUIRE(parseDecimal("100", 2) == 10000);
    REQUIRE(parseDecimal("-5.5", 2) == -550);
    REQUIRE(parseDecimal("0.00", 2) == 0);
    REQUIRE(parseDecimal("1.0", 2) == 100);
    REQUIRE(parseDecimal("0.1", 2) == 10);
    REQUIRE(parseDecimal("999.99", 2) == 99999);
    REQUIRE(parseDecimal("-0.01", 2) == -1);
    REQUIRE(parseDecimal("12345678.90", 2) == 1234567890);
}

// ── parseDate tests ──────────────────────────────────────────────────────────

TEST_CASE("parseDate", "[storage]") {
    REQUIRE(parseDate("2000-01-01") == 0);
    REQUIRE(parseDate("2000-01-02") == 1);
    REQUIRE(parseDate("1999-12-31") == -1);
    REQUIRE(parseDate("2000-02-29") == 59);  // 2000 is a leap year
    REQUIRE(parseDate("2000-03-01") == 60);

    // 1998-09-02: should be negative
    int32_t d = parseDate("1998-09-02");
    REQUIRE(d < 0);
    REQUIRE(d == -486);

    // 1996-03-13: should be negative
    int32_t d2 = parseDate("1996-03-13");
    REQUIRE(d2 < 0);

    // A date after epoch
    REQUIRE(parseDate("2001-01-01") == 366);  // 2000 is a leap year, so 366 days
}

// ── lineitemTableDef tests ───────────────────────────────────────────────────

TEST_CASE("lineitemTableDef", "[storage]") {
    auto def = lineitemTableDef();
    REQUIRE(def.name == "lineitem");
    REQUIRE(def.columns.size() == 16);

    // Spot-check column names and types
    REQUIRE(def.columns[0].name == "l_orderkey");
    REQUIRE(def.columns[0].type.id == SQLTypeId::INTEGER);
    REQUIRE(def.columns[0].columnIndex == 0);

    REQUIRE(def.columns[4].name == "l_quantity");
    REQUIRE(def.columns[4].type.id == SQLTypeId::DECIMAL);
    REQUIRE(def.columns[4].type.precision == 15);
    REQUIRE(def.columns[4].type.scale == 2);

    REQUIRE(def.columns[8].name == "l_returnflag");
    REQUIRE(def.columns[8].type.id == SQLTypeId::VARCHAR);
    REQUIRE(def.columns[8].type.maxLen == 1);

    REQUIRE(def.columns[10].name == "l_shipdate");
    REQUIRE(def.columns[10].type.id == SQLTypeId::DATE);

    REQUIRE(def.columns[15].name == "l_comment");
    REQUIRE(def.columns[15].type.id == SQLTypeId::VARCHAR);
    REQUIRE(def.columns[15].type.maxLen == 44);
    REQUIRE(def.columns[15].columnIndex == 15);
}

// ── Load TBL file tests ─────────────────────────────────────────────────────

TEST_CASE("Load TBL file", "[storage]") {
    // Create a temporary .tbl file with realistic lineitem data
    std::string tmpPath = "test_lineitem.tbl";
    {
        std::ofstream out(tmpPath);
        REQUIRE(out.is_open());
        // 3 rows of lineitem data (16 fields each, trailing '|')
        out << "1|155190|7706|1|17.00|21168.23|0.04|0.02|N|O|1996-03-13|1996-02-12|"
               "1996-03-22|DELIVER IN PERSON|TRUCK|egular courts above the|\n";
        out << "1|67310|7311|2|36.00|45983.16|0.09|0.06|N|O|1996-04-12|1996-02-28|"
               "1996-04-20|TAKE BACK RETURN|MAIL|ly final dependencies|\n";
        out << "1|63700|3701|3|8.00|13309.60|0.10|0.02|N|O|1996-01-29|1996-03-05|"
               "1996-01-31|TAKE BACK RETURN|REG AIR|riously. regular express|\n";
    }

    auto schema = lineitemTableDef();
    auto table = loadTblFile(tmpPath, schema);

    // Cleanup temp file
    std::remove(tmpPath.c_str());

    REQUIRE(table.rowCount == 3);
    REQUIRE(table.columns.size() == 16);

    // Check l_orderkey (column 0, INTEGER)
    auto* orderKeys = static_cast<int32_t*>(table.columnPtrs()[0]);
    REQUIRE(orderKeys[0] == 1);
    REQUIRE(orderKeys[1] == 1);
    REQUIRE(orderKeys[2] == 1);

    // Check l_partkey (column 1, INTEGER)
    auto* partKeys = static_cast<int32_t*>(table.columnPtrs()[1]);
    REQUIRE(partKeys[0] == 155190);
    REQUIRE(partKeys[1] == 67310);
    REQUIRE(partKeys[2] == 63700);

    // Check l_quantity (column 4, DECIMAL) — "17.00" → 1700
    auto* quantity = static_cast<int64_t*>(table.columnPtrs()[4]);
    REQUIRE(quantity[0] == 1700);
    REQUIRE(quantity[1] == 3600);
    REQUIRE(quantity[2] == 800);

    // Check l_extendedprice (column 5, DECIMAL) — "21168.23" → 2116823
    auto* price = static_cast<int64_t*>(table.columnPtrs()[5]);
    REQUIRE(price[0] == 2116823);
    REQUIRE(price[1] == 4598316);
    REQUIRE(price[2] == 1330960);

    // Check l_discount (column 6, DECIMAL) — "0.04" → 4
    auto* discount = static_cast<int64_t*>(table.columnPtrs()[6]);
    REQUIRE(discount[0] == 4);
    REQUIRE(discount[1] == 9);
    REQUIRE(discount[2] == 10);

    // Check l_returnflag (column 8, VARCHAR) — should be "N" for all
    auto* returnFlag = static_cast<StringValue*>(table.columnPtrs()[8]);
    for (int i = 0; i < 3; ++i) {
        std::string_view sv(table.stringBuffer.data() + returnFlag[i].offset, returnFlag[i].len);
        REQUIRE(sv == "N");
    }

    // Check l_linestatus (column 9, VARCHAR) — should be "O" for all
    auto* lineStatus = static_cast<StringValue*>(table.columnPtrs()[9]);
    for (int i = 0; i < 3; ++i) {
        std::string_view sv(table.stringBuffer.data() + lineStatus[i].offset, lineStatus[i].len);
        REQUIRE(sv == "O");
    }

    // Check l_shipdate (column 10, DATE)
    auto* shipDate = static_cast<int32_t*>(table.columnPtrs()[10]);
    REQUIRE(shipDate[0] == parseDate("1996-03-13"));
    REQUIRE(shipDate[1] == parseDate("1996-04-12"));
    REQUIRE(shipDate[2] == parseDate("1996-01-29"));

    // Check l_shipinstruct (column 13, VARCHAR)
    auto* shipInstruct = static_cast<StringValue*>(table.columnPtrs()[13]);
    std::string_view si0(table.stringBuffer.data() + shipInstruct[0].offset, shipInstruct[0].len);
    REQUIRE(si0 == "DELIVER IN PERSON");
    std::string_view si1(table.stringBuffer.data() + shipInstruct[1].offset, shipInstruct[1].len);
    REQUIRE(si1 == "TAKE BACK RETURN");

    // Check l_shipmode (column 14, VARCHAR)
    auto* shipMode = static_cast<StringValue*>(table.columnPtrs()[14]);
    std::string_view sm0(table.stringBuffer.data() + shipMode[0].offset, shipMode[0].len);
    REQUIRE(sm0 == "TRUCK");
    std::string_view sm1(table.stringBuffer.data() + shipMode[1].offset, shipMode[1].len);
    REQUIRE(sm1 == "MAIL");
    std::string_view sm2(table.stringBuffer.data() + shipMode[2].offset, shipMode[2].len);
    REQUIRE(sm2 == "REG AIR");

    // Check l_comment (column 15, VARCHAR)
    auto* comment = static_cast<StringValue*>(table.columnPtrs()[15]);
    std::string_view c0(table.stringBuffer.data() + comment[0].offset, comment[0].len);
    REQUIRE(c0 == "egular courts above the");
}

// ── RuntimeContext tests ─────────────────────────────────────────────────────

TEST_CASE("RuntimeContext initialization", "[storage]") {
    RuntimeContext ctx;
    REQUIRE(ctx.columnPtrs == nullptr);
    REQUIRE(ctx.rowCount == 0);
    REQUIRE(ctx.stringBase == nullptr);
    REQUIRE(ctx.hashTable == nullptr);
    REQUIRE(ctx.resultBuffer == nullptr);
    REQUIRE(ctx.resultRowCount == 0);
}

TEST_CASE("RuntimeContext wiring with Table", "[storage]") {
    // Build a small table and wire it into a RuntimeContext
    Table table;
    table.name = "test";
    table.columns.push_back(Column::create(SQLTypeId::INTEGER));
    auto& vec = std::get<std::vector<int32_t>>(table.columns[0].storage);
    vec.push_back(10);
    vec.push_back(20);
    table.finalize();

    RuntimeContext ctx;
    // const_cast is needed because Table stores void* but RuntimeContext wants void**
    ctx.columnPtrs = const_cast<void**>(table.columnPtrs());
    ctx.rowCount = table.rowCount;
    ctx.stringBase = table.stringBuffer.data();

    REQUIRE(ctx.rowCount == 2);
    auto* intPtr = static_cast<int32_t*>(ctx.columnPtrs[0]);
    REQUIRE(intPtr[0] == 10);
    REQUIRE(intPtr[1] == 20);
}
