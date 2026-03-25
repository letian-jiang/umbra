#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "codegen/tuples/tuple_layout.h"
#include "common/types.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::codegen;
using namespace umbra::ir;
using namespace umbra;
using umbra::interpreter::Interpreter;

// ── Helper ──────────────────────────────────────────────────────────────────

static uint64_t run(Program& program, FunctionId func, std::vector<uint64_t> args = {}) {
    Interpreter interp;
    return interp.execute(program, func, std::span<const uint64_t>(args));
}

// ── Layout computation ──────────────────────────────────────────────────────

TEST_CASE("TupleLayout: single Int64 field", "[tuples]") {
    TupleLayout layout({SQLType::bigint()});
    REQUIRE(layout.numFields() == 1);
    REQUIRE(layout.field(0).offset == 0);
    REQUIRE(layout.field(0).irType == TypeId::INT64);
    REQUIRE(layout.nullBitmapOffset() == 8);
    REQUIRE(layout.nullBitmapSize() == 1);
    REQUIRE(layout.totalSize() == 9);
}

TEST_CASE("TupleLayout: two Int8 fields with null bitmap", "[tuples]") {
    TupleLayout layout({SQLType::boolean(), SQLType::boolean()});
    REQUIRE(layout.numFields() == 2);
    REQUIRE(layout.field(0).offset == 0);
    REQUIRE(layout.field(1).offset == 1);
    REQUIRE(layout.nullBitmapOffset() == 2);
    REQUIRE(layout.nullBitmapSize() == 1);
    REQUIRE(layout.totalSize() == 3);
}

TEST_CASE("TupleLayout: mixed types with alignment", "[tuples]") {
    // Int8, Int64, Int32 — alignment should pad after Int8
    TupleLayout layout({SQLType::boolean(), SQLType::bigint(), SQLType::integer()});
    REQUIRE(layout.numFields() == 3);
    // Field 0: Int8 at offset 0
    REQUIRE(layout.field(0).offset == 0);
    REQUIRE(layout.field(0).irType == TypeId::BOOL);
    // Field 1: Int64 at offset 8 (aligned to 8)
    REQUIRE(layout.field(1).offset == 8);
    REQUIRE(layout.field(1).irType == TypeId::INT64);
    // Field 2: Int32 at offset 16 (aligned to 4, 16 is already aligned)
    REQUIRE(layout.field(2).offset == 16);
    REQUIRE(layout.field(2).irType == TypeId::INT32);
    // Null bitmap at offset 20
    REQUIRE(layout.nullBitmapOffset() == 20);
    REQUIRE(layout.nullBitmapSize() == 1);
    REQUIRE(layout.totalSize() == 21);
}

TEST_CASE("TupleLayout: Q1 group-by keys (returnflag + linestatus)", "[tuples]") {
    // Both are Int8 (BOOLEAN maps to INT8 for storage)
    // In Q1, returnflag and linestatus are single chars stored as Int8
    TupleLayout layout({SQLType::boolean(), SQLType::boolean()});
    REQUIRE(layout.totalSize() == 3);  // 1 + 1 + 1 (bitmap)
}

TEST_CASE("TupleLayout: 9 fields for null bitmap size", "[tuples]") {
    // 9 fields need 2 bytes for null bitmap
    std::vector<SQLType> types(9, SQLType::boolean());
    TupleLayout layout(types);
    REQUIRE(layout.nullBitmapSize() == 2);
}

TEST_CASE("TupleLayout: empty layout", "[tuples]") {
    TupleLayout layout({});
    REQUIRE(layout.numFields() == 0);
    REQUIRE(layout.totalSize() == 0);
    REQUIRE(layout.nullBitmapSize() == 0);
}

// ── Pack/Unpack round-trip ──────────────────────────────────────────────────

TEST_CASE("TupleLayout: pack/unpack single Int64 round-trip", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::bigint()});

    // fn(bufPtr, value) -> void: pack the value, then unpack and return it.
    auto fn = ctx.createFunction("pack_unpack_i64", TypeId::INT64, {TypeId::PTR, TypeId::INT64});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);
    auto valueOp = ctx.builder().emitFunArg(TypeId::INT64, 1);
    SQLValue val(ctx, valueOp, SQLType::bigint());

    layout.pack(ctx, bufPtr, {val});
    auto unpacked = layout.unpack(ctx, bufPtr);
    REQUIRE(unpacked.size() == 1);
    ctx.builder().emitRet(TypeId::INT64, unpacked[0].value());

    auto program = ctx.finalize();

    // Allocate buffer for the tuple.
    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(static_cast<int64_t>(run(program, fn, {bufAddr, 42})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, fn, {bufAddr, static_cast<uint64_t>(-100)})) == -100);
}

TEST_CASE("TupleLayout: pack/unpack two Int8 fields", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::boolean(), SQLType::boolean()});

    // fn(bufPtr, v1, v2) -> i64: pack two Int8s, unpack, return v1 + v2*256.
    auto fn = ctx.createFunction("pack_unpack_2i8", TypeId::INT64,
                                 {TypeId::PTR, TypeId::INT8, TypeId::INT8});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);
    auto v1Op = ctx.builder().emitFunArg(TypeId::INT8, 1);
    auto v2Op = ctx.builder().emitFunArg(TypeId::INT8, 2);
    SQLValue val1(ctx, v1Op, SQLType::boolean());
    SQLValue val2(ctx, v2Op, SQLType::boolean());

    layout.pack(ctx, bufPtr, {val1, val2});
    auto unpacked = layout.unpack(ctx, bufPtr);
    REQUIRE(unpacked.size() == 2);

    // Combine: zext both to Int64, return v1 + v2 * 256
    auto v1Ext = ctx.builder().emitZExt(TypeId::INT64, unpacked[0].value());
    auto v2Ext = ctx.builder().emitZExt(TypeId::INT64, unpacked[1].value());
    auto mul = ctx.builder().emitConst(TypeId::INT64, 256);
    auto v2Shifted = ctx.builder().emitMul(TypeId::INT64, v2Ext, mul);
    auto result = ctx.builder().emitAdd(TypeId::INT64, v1Ext, v2Shifted);
    ctx.builder().emitRet(TypeId::INT64, result);

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    // v1=65('A'), v2=82('R')
    REQUIRE(run(program, fn, {bufAddr, 65, 82}) == 65 + 82 * 256);
}

TEST_CASE("TupleLayout: pack/unpack mixed types with alignment", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::boolean(), SQLType::bigint()});

    // fn(bufPtr, boolVal, i64Val) -> i64: pack, unpack, return i64 value.
    auto fn = ctx.createFunction("pack_unpack_mixed", TypeId::INT64,
                                 {TypeId::PTR, TypeId::INT8, TypeId::INT64});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);
    auto boolOp = ctx.builder().emitFunArg(TypeId::INT8, 1);
    auto i64Op = ctx.builder().emitFunArg(TypeId::INT64, 2);
    SQLValue boolVal(ctx, boolOp, SQLType::boolean());
    SQLValue i64Val(ctx, i64Op, SQLType::bigint());

    layout.pack(ctx, bufPtr, {boolVal, i64Val});
    auto unpacked = layout.unpack(ctx, bufPtr);
    REQUIRE(unpacked.size() == 2);
    ctx.builder().emitRet(TypeId::INT64, unpacked[1].value());

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(static_cast<int64_t>(run(program, fn, {bufAddr, 1, 12345})) == 12345);
}

TEST_CASE("TupleLayout: null flag round-trip", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::bigint()});

    // fn(bufPtr) -> bool: pack a null value, unpack, return null flag.
    auto fn = ctx.createFunction("null_roundtrip", TypeId::BOOL, {TypeId::PTR});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);

    auto nullVal = SQLValue::makeNull(ctx, SQLType::bigint());
    layout.pack(ctx, bufPtr, {nullVal});
    auto unpacked = layout.unpack(ctx, bufPtr);
    ctx.builder().emitRet(TypeId::BOOL, unpacked[0].nullFlag());

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(run(program, fn, {bufAddr}) == 1);
}

TEST_CASE("TupleLayout: non-null flag round-trip", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::bigint()});

    auto fn = ctx.createFunction("nonnull_roundtrip", TypeId::BOOL, {TypeId::PTR});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);

    auto val = SQLValue::makeConst(ctx, 42, SQLType::bigint());
    layout.pack(ctx, bufPtr, {val});
    auto unpacked = layout.unpack(ctx, bufPtr);
    ctx.builder().emitRet(TypeId::BOOL, unpacked[0].nullFlag());

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(run(program, fn, {bufAddr}) == 0);
}

TEST_CASE("TupleLayout: multiple null flags in bitmap", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::bigint(), SQLType::bigint()});

    // Pack: first non-null, second null. Check both flags.
    auto fn = ctx.createFunction("multi_null", TypeId::INT64, {TypeId::PTR});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);

    auto val1 = SQLValue::makeConst(ctx, 100, SQLType::bigint());
    auto val2 = SQLValue::makeNull(ctx, SQLType::bigint());
    layout.pack(ctx, bufPtr, {val1, val2});
    auto unpacked = layout.unpack(ctx, bufPtr);

    // Return null1 + null2*2 (should be 0 + 1*2 = 2)
    auto n1 = ctx.builder().emitZExt(TypeId::INT64, unpacked[0].nullFlag());
    auto n2 = ctx.builder().emitZExt(TypeId::INT64, unpacked[1].nullFlag());
    auto two = ctx.builder().emitConst(TypeId::INT64, 2);
    auto n2Shifted = ctx.builder().emitMul(TypeId::INT64, n2, two);
    auto result = ctx.builder().emitAdd(TypeId::INT64, n1, n2Shifted);
    ctx.builder().emitRet(TypeId::INT64, result);

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(run(program, fn, {bufAddr}) == 2);
}

// ── Hash ────────────────────────────────────────────────────────────────────

TEST_CASE("TupleLayout: hash produces consistent values", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Hash a single Int64 value.
    auto fn = ctx.createFunction("hash_test", TypeId::UINT64, {TypeId::INT64});
    auto valOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    SQLValue val(ctx, valOp, SQLType::bigint());

    auto h = TupleLayout::hash(ctx, {val});
    ctx.emitRet(h);

    auto program = ctx.finalize();

    // Same input should produce same hash.
    auto h1 = run(program, fn, {42});
    auto h2 = run(program, fn, {42});
    REQUIRE(h1 == h2);

    // Different input should (very likely) produce different hash.
    auto h3 = run(program, fn, {43});
    REQUIRE(h1 != h3);
}

TEST_CASE("TupleLayout: hash of two Int8 keys", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Hash two Int8 keys (like Q1 returnflag + linestatus).
    auto fn = ctx.createFunction("hash_2keys", TypeId::UINT64, {TypeId::INT8, TypeId::INT8});
    auto k1Op = ctx.builder().emitFunArg(TypeId::INT8, 0);
    auto k2Op = ctx.builder().emitFunArg(TypeId::INT8, 1);
    SQLValue key1(ctx, k1Op, SQLType::boolean());
    SQLValue key2(ctx, k2Op, SQLType::boolean());

    auto h = TupleLayout::hash(ctx, {key1, key2});
    ctx.emitRet(h);

    auto program = ctx.finalize();

    // Different key combinations should produce different hashes.
    auto hAF = run(program, fn, {'A', 'F'});
    auto hAO = run(program, fn, {'A', 'O'});
    auto hNF = run(program, fn, {'N', 'F'});
    auto hRF = run(program, fn, {'R', 'F'});
    REQUIRE(hAF != hAO);
    REQUIRE(hAF != hNF);
    REQUIRE(hAF != hRF);
    REQUIRE(hAO != hNF);
}

TEST_CASE("TupleLayout: hash non-zero for any input", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("hash_nonzero", TypeId::UINT64, {TypeId::INT64});
    auto valOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    SQLValue val(ctx, valOp, SQLType::bigint());
    auto h = TupleLayout::hash(ctx, {val});
    ctx.emitRet(h);

    auto program = ctx.finalize();

    // Even for input 0, hash should be non-zero.
    REQUIRE(run(program, fn, {0}) != 0);
}

// ── DECIMAL pack/unpack ─────────────────────────────────────────────────────

TEST_CASE("TupleLayout: DECIMAL pack/unpack round-trip", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::decimal(15, 2)});

    auto fn = ctx.createFunction("dec_roundtrip", TypeId::INT64, {TypeId::PTR, TypeId::INT64});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);
    auto valOp = ctx.builder().emitFunArg(TypeId::INT64, 1);
    SQLValue val(ctx, valOp, SQLType::decimal(15, 2));

    layout.pack(ctx, bufPtr, {val});
    auto unpacked = layout.unpack(ctx, bufPtr);
    ctx.builder().emitRet(TypeId::INT64, unpacked[0].value());

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    // 123.45 as DECIMAL(15,2) = 12345
    REQUIRE(static_cast<int64_t>(run(program, fn, {bufAddr, 12345})) == 12345);
}

// ── Int32 (DATE) pack/unpack ────────────────────────────────────────────────

TEST_CASE("TupleLayout: DATE (Int32) pack/unpack round-trip", "[tuples]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    TupleLayout layout({SQLType::date()});

    auto fn = ctx.createFunction("date_roundtrip", TypeId::INT32, {TypeId::PTR, TypeId::INT32});
    auto bufPtr = ctx.getArg<Ptr<Int8>>(0);
    auto valOp = ctx.builder().emitFunArg(TypeId::INT32, 1);
    SQLValue val(ctx, valOp, SQLType::date());

    layout.pack(ctx, bufPtr, {val});
    auto unpacked = layout.unpack(ctx, bufPtr);
    ctx.builder().emitRet(TypeId::INT32, unpacked[0].value());

    auto program = ctx.finalize();

    std::vector<uint8_t> buf(layout.totalSize(), 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto bufAddr = reinterpret_cast<uint64_t>(buf.data());
    REQUIRE(static_cast<int32_t>(run(program, fn, {bufAddr, 365})) == 365);
}
