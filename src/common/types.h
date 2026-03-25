#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace umbra {

/// SQL type enumeration
enum class SQLTypeId {
    INTEGER,  // int32_t
    BIGINT,   // int64_t
    DATE,     // int32_t (days since 2000-01-01)
    DECIMAL,  // int64_t (scaled integer)
    VARCHAR,  // StringRef
    BOOLEAN,  // int8_t (internal predicates)
};

/// Full type descriptor
struct SQLType {
    SQLTypeId id;
    uint8_t precision{0};  // For DECIMAL
    uint8_t scale{0};      // For DECIMAL
    uint16_t maxLen{0};    // For VARCHAR

    static SQLType integer() { return {SQLTypeId::INTEGER}; }
    static SQLType bigint() { return {SQLTypeId::BIGINT}; }
    static SQLType date() { return {SQLTypeId::DATE}; }
    static SQLType decimal(uint8_t p, uint8_t s) { return {SQLTypeId::DECIMAL, p, s}; }
    static SQLType varchar(uint16_t len) { return {SQLTypeId::VARCHAR, 0, 0, len}; }
    static SQLType boolean() { return {SQLTypeId::BOOLEAN}; }
};

/// String reference (non-owning pointer + length, for runtime use)
struct StringRef {
    const char* data{nullptr};
    uint32_t length{0};

    [[nodiscard]] std::string_view view() const { return {data, length}; }
    [[nodiscard]] std::string toString() const { return {data, length}; }
};

/// Storage format for VARCHAR columns: offset into Table.string_buffer
struct StringValue {
    uint32_t offset;  // byte offset into Table.string_buffer
    uint32_t len;     // byte length
};

/// Date epoch: 2000-01-01
constexpr int32_t DATE_EPOCH_YEAR = 2000;
constexpr int32_t DATE_EPOCH_MONTH = 1;
constexpr int32_t DATE_EPOCH_DAY = 1;

/// Size of each type in bytes (for storage)
inline size_t typeSize(SQLTypeId id) {
    switch (id) {
        case SQLTypeId::INTEGER:
            return sizeof(int32_t);
        case SQLTypeId::BIGINT:
            return sizeof(int64_t);
        case SQLTypeId::DATE:
            return sizeof(int32_t);
        case SQLTypeId::DECIMAL:
            return sizeof(int64_t);
        case SQLTypeId::VARCHAR:
            return sizeof(StringValue);
        case SQLTypeId::BOOLEAN:
            return sizeof(int8_t);
    }
    return 0;  // unreachable
}

}  // namespace umbra
