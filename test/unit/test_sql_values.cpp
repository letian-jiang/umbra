#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "common/types.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::codegen;
using namespace umbra::ir;
using namespace umbra;
using umbra::interpreter::Interpreter;

// ── Helper ──────────────────────────────────────────────────────────────────

/// Execute a single-function program and return the result.
static uint64_t run(Program& program, FunctionId func, std::vector<uint64_t> args = {}) {
    Interpreter interp;
    return interp.execute(program, func, std::span<const uint64_t>(args));
}

// ── SQL Type to IR mapping ──────────────────────────────────────────────────

TEST_CASE("SQLValue: sqlTypeToIR mapping", "[sql_values]") {
    REQUIRE(sqlTypeToIR(SQLTypeId::BOOLEAN) == TypeId::BOOL);
    REQUIRE(sqlTypeToIR(SQLTypeId::INTEGER) == TypeId::INT32);
    REQUIRE(sqlTypeToIR(SQLTypeId::DATE) == TypeId::INT32);
    REQUIRE(sqlTypeToIR(SQLTypeId::BIGINT) == TypeId::INT64);
    REQUIRE(sqlTypeToIR(SQLTypeId::DECIMAL) == TypeId::INT64);
    REQUIRE(sqlTypeToIR(SQLTypeId::VARCHAR) == TypeId::PTR);
}

// ── Non-null arithmetic ─────────────────────────────────────────────────────

TEST_CASE("SQLValue: BIGINT add", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("bigint_add", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue b(ctx, bOp, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {10, 20})) == 30);
    REQUIRE(static_cast<int64_t>(run(program, fn, {static_cast<uint64_t>(-5), 3})) == -2);
}

TEST_CASE("SQLValue: BIGINT sub/mul/div", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("bigint_ops", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue b(ctx, bOp, SQLType::bigint());

    // (a - b) * (a + b) / b
    auto diff = SQLValue::evaluateBinary(ctx, BinaryOp::SUB, a, b);
    auto sum = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    auto product = SQLValue::evaluateBinary(ctx, BinaryOp::MUL, diff, sum);
    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::DIV, product, b);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    // (10-3)*(10+3)/3 = 7*13/3 = 91/3 = 30
    REQUIRE(static_cast<int64_t>(run(program, fn, {10, 3})) == 30);
}

TEST_CASE("SQLValue: INTEGER add with promotion", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // INTEGER + BIGINT -> BIGINT (promotion)
    auto fn = ctx.createFunction("int_bigint_add", TypeId::INT64, {TypeId::INT32, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT32, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::integer());
    SQLValue b(ctx, bOp, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {100, 200})) == 300);
}

// ── DECIMAL arithmetic ──────────────────────────────────────────────────────

TEST_CASE("SQLValue: DECIMAL add (same scale)", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // DECIMAL(10,2) + DECIMAL(10,2) → DECIMAL(10,2)
    // Values are scaled by 100, so 1.50 = 150, 2.25 = 225
    auto fn = ctx.createFunction("dec_add", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 2));
    SQLValue b(ctx, bOp, SQLType::decimal(10, 2));

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    // 1.50 + 2.25 = 3.75 → 150 + 225 = 375
    REQUIRE(static_cast<int64_t>(run(program, fn, {150, 225})) == 375);
}

TEST_CASE("SQLValue: DECIMAL multiply with rescaling", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // DECIMAL(10,2) * DECIMAL(10,2) → DECIMAL(10,2)
    // Raw multiply: 150 * 225 = 33750 (effective scale 4)
    // Rescale to scale 2: 33750 / 100 = 337 (truncated: 3.37)
    auto fn = ctx.createFunction("dec_mul", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 2));
    SQLValue b(ctx, bOp, SQLType::decimal(10, 2));

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::MUL, a, b);
    REQUIRE(result.type().scale == 2);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    // 1.50 * 2.25: raw = 150 * 225 = 33750, rescale /100 = 337
    REQUIRE(static_cast<int64_t>(run(program, fn, {150, 225})) == 337);

    // 10.00 * 3.00: raw = 1000 * 300 = 300000, rescale /100 = 3000 → 30.00
    REQUIRE(static_cast<int64_t>(run(program, fn, {1000, 300})) == 3000);
}

TEST_CASE("SQLValue: DECIMAL division", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // DECIMAL(10,2) / DECIMAL(10,2) → DECIMAL(10,2)
    // To preserve scale: (lhs * 10^rhs_scale) / rhs
    // 10.00 / 4.00 = 2.50 → (1000 * 100) / 400 = 250
    auto fn = ctx.createFunction("dec_div", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 2));
    SQLValue b(ctx, bOp, SQLType::decimal(10, 2));

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::DIV, a, b);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    // 10.00 / 4.00: (1000 * 100) / 400 = 250 → 2.50
    REQUIRE(static_cast<int64_t>(run(program, fn, {1000, 400})) == 250);
    // 9.00 / 3.00: (900 * 100) / 300 = 300 → 3.00
    REQUIRE(static_cast<int64_t>(run(program, fn, {900, 300})) == 300);
}

TEST_CASE("SQLValue: DECIMAL / BIGINT (AVG pattern)", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // SUM(DECIMAL(10,2)) / COUNT(BIGINT) → DECIMAL(10,2)
    // avg_qty = sum_qty / count
    auto fn = ctx.createFunction("dec_div_bigint", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto sumOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto countOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue sumVal(ctx, sumOp, SQLType::decimal(10, 2));
    SQLValue countVal(ctx, countOp, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::DIV, sumVal, countVal);
    REQUIRE(result.type().id == SQLTypeId::DECIMAL);
    REQUIRE(result.type().scale == 2);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    // sum=1500 (15.00), count=3 → 1500/3 = 500 → 5.00
    REQUIRE(static_cast<int64_t>(run(program, fn, {1500, 3})) == 500);
    // sum=1000 (10.00), count=4 → 1000/4 = 250 → 2.50
    REQUIRE(static_cast<int64_t>(run(program, fn, {1000, 4})) == 250);
}

TEST_CASE("SQLValue: TPC-H Q1 discount pattern", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // l_extendedprice * (1 - l_discount)
    // extendedprice: DECIMAL(15,2), discount: DECIMAL(15,2)
    // 1.00 in DECIMAL(15,2) = 100
    auto fn = ctx.createFunction("disc_pattern", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto priceOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto discOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue price(ctx, priceOp, SQLType::decimal(15, 2));
    SQLValue discount(ctx, discOp, SQLType::decimal(15, 2));
    SQLValue one = SQLValue::makeConst(ctx, 100, SQLType::decimal(15, 2));

    // (1 - discount)
    auto oneMinusDisc = SQLValue::evaluateBinary(ctx, BinaryOp::SUB, one, discount);
    // price * (1 - discount)
    auto discPrice = SQLValue::evaluateBinary(ctx, BinaryOp::MUL, price, oneMinusDisc);

    ctx.builder().emitRet(TypeId::INT64, discPrice.value());

    auto program = ctx.finalize();
    // price=100.00 (10000), discount=0.05 (5)
    // (100 - 5) = 95, 10000 * 95 = 950000, /100 = 9500 → 95.00
    REQUIRE(static_cast<int64_t>(run(program, fn, {10000, 5})) == 9500);
    // price=50.00 (5000), discount=0.10 (10)
    // (100 - 10) = 90, 5000 * 90 = 450000, /100 = 4500 → 45.00
    REQUIRE(static_cast<int64_t>(run(program, fn, {5000, 10})) == 4500);
}

// ── Comparison ──────────────────────────────────────────────────────────────

TEST_CASE("SQLValue: DATE comparison (INT32)", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("date_cmp", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT32, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT32, 1);

    SQLValue a(ctx, aOp, SQLType::date());
    SQLValue b(ctx, bOp, SQLType::date());

    auto result = SQLValue::evaluateCompare(ctx, CmpOp::LE, a, b);
    REQUIRE(result.type().id == SQLTypeId::BOOLEAN);
    ctx.builder().emitRet(TypeId::BOOL, result.value());

    auto program = ctx.finalize();
    // 100 <= 200 → true
    REQUIRE(run(program, fn, {100, 200}) == 1);
    // 200 <= 100 → false
    REQUIRE(run(program, fn, {200, 100}) == 0);
    // 100 <= 100 → true
    REQUIRE(run(program, fn, {100, 100}) == 1);
}

TEST_CASE("SQLValue: BIGINT equality comparison", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("bigint_eq", TypeId::BOOL, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue b(ctx, bOp, SQLType::bigint());

    auto result = SQLValue::evaluateCompare(ctx, CmpOp::EQ, a, b);
    ctx.builder().emitRet(TypeId::BOOL, result.value());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {42, 42}) == 1);
    REQUIRE(run(program, fn, {42, 43}) == 0);
}

TEST_CASE("SQLValue: DECIMAL comparison (different ops)", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("dec_cmp", TypeId::BOOL, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 2));
    SQLValue b(ctx, bOp, SQLType::decimal(10, 2));

    auto result = SQLValue::evaluateCompare(ctx, CmpOp::GT, a, b);
    ctx.builder().emitRet(TypeId::BOOL, result.value());

    auto program = ctx.finalize();
    // 3.50 > 2.50 → true (350 > 250)
    REQUIRE(run(program, fn, {350, 250}) == 1);
    // 2.50 > 3.50 → false
    REQUIRE(run(program, fn, {250, 350}) == 0);
}

// ── NULL propagation ────────────────────────────────────────────────────────

TEST_CASE("SQLValue: NULL propagation in arithmetic", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Return the null flag of (a + null)
    auto fn = ctx.createFunction("null_prop_add", TypeId::BOOL, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue nullVal = SQLValue::makeNull(ctx, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, nullVal);
    // The null flag should be true
    ctx.builder().emitRet(TypeId::BOOL, result.nullFlag());

    auto program = ctx.finalize();
    // Any value + NULL = NULL (null flag = true)
    REQUIRE(run(program, fn, {42}) == 1);
    REQUIRE(run(program, fn, {0}) == 1);
}

TEST_CASE("SQLValue: non-null + non-null has null_flag=false", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("not_null_add", TypeId::BOOL, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue b(ctx, bOp, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    ctx.builder().emitRet(TypeId::BOOL, result.nullFlag());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {1, 2}) == 0);
}

TEST_CASE("SQLValue: NULL propagation in comparison", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("null_prop_cmp", TypeId::BOOL, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue nullVal = SQLValue::makeNull(ctx, SQLType::bigint());

    auto result = SQLValue::evaluateCompare(ctx, CmpOp::LT, a, nullVal);
    // null flag should be true
    ctx.builder().emitRet(TypeId::BOOL, result.nullFlag());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {42}) == 1);
}

TEST_CASE("SQLValue: NULL + NULL has null_flag=true", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("null_null_add", TypeId::BOOL, {});

    SQLValue a = SQLValue::makeNull(ctx, SQLType::bigint());
    SQLValue b = SQLValue::makeNull(ctx, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, a, b);
    ctx.builder().emitRet(TypeId::BOOL, result.nullFlag());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn) == 1);
}

// ── Cast ────────────────────────────────────────────────────────────────────

TEST_CASE("SQLValue: cast INTEGER to BIGINT", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_int_bigint", TypeId::INT64, {TypeId::INT32});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT32, 0);

    SQLValue a(ctx, aOp, SQLType::integer());
    auto casted = a.castTo(ctx, SQLType::bigint());
    REQUIRE(casted.type().id == SQLTypeId::BIGINT);
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {42})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, fn, {static_cast<uint64_t>(-10)})) == -10);
}

TEST_CASE("SQLValue: cast INTEGER to DECIMAL", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_int_dec", TypeId::INT64, {TypeId::INT32});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT32, 0);

    SQLValue a(ctx, aOp, SQLType::integer());
    auto casted = a.castTo(ctx, SQLType::decimal(10, 2));
    REQUIRE(casted.type().id == SQLTypeId::DECIMAL);
    REQUIRE(casted.type().scale == 2);
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    // 42 → 4200 (scaled by 100)
    REQUIRE(static_cast<int64_t>(run(program, fn, {42})) == 4200);
}

TEST_CASE("SQLValue: cast BIGINT to DECIMAL", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_bigint_dec", TypeId::INT64, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::bigint());
    auto casted = a.castTo(ctx, SQLType::decimal(10, 2));
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    // 7 → 700
    REQUIRE(static_cast<int64_t>(run(program, fn, {7})) == 700);
}

TEST_CASE("SQLValue: cast preserves null flag", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_null", TypeId::BOOL, {});

    SQLValue a = SQLValue::makeNull(ctx, SQLType::integer());
    auto casted = a.castTo(ctx, SQLType::bigint());
    ctx.builder().emitRet(TypeId::BOOL, casted.nullFlag());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn) == 1);
}

// ── makeConst ───────────────────────────────────────────────────────────────

TEST_CASE("SQLValue: makeConst creates non-null value", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("make_const", TypeId::INT64, {});
    auto val = SQLValue::makeConst(ctx, 42, SQLType::bigint());
    ctx.builder().emitRet(TypeId::INT64, val.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn)) == 42);
}

TEST_CASE("SQLValue: makeConst null flag is false", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("make_const_null", TypeId::BOOL, {});
    auto val = SQLValue::makeConst(ctx, 42, SQLType::bigint());
    ctx.builder().emitRet(TypeId::BOOL, val.nullFlag());

    auto program = ctx.finalize();
    REQUIRE(run(program, fn) == 0);
}

// ── DECIMAL scale conversion ────────────────────────────────────────────────

TEST_CASE("SQLValue: DECIMAL scale up conversion", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("scale_up", TypeId::INT64, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 1));
    auto casted = a.castTo(ctx, SQLType::decimal(10, 3));
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    // 1.5 (scale 1 = 15) → scale 3 = 1500
    REQUIRE(static_cast<int64_t>(run(program, fn, {15})) == 1500);
}

TEST_CASE("SQLValue: DECIMAL scale down conversion", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("scale_down", TypeId::INT64, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::decimal(10, 4));
    auto casted = a.castTo(ctx, SQLType::decimal(10, 2));
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    // 1.2345 (scale 4 = 12345) → scale 2 = 12345/100 = 123 (truncated: 1.23)
    REQUIRE(static_cast<int64_t>(run(program, fn, {12345})) == 123);
}

// ── COUNT pattern ───────────────────────────────────────────────────────────

TEST_CASE("SQLValue: COUNT(*) increment pattern", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Simulate: count += 1
    auto fn = ctx.createFunction("count_inc", TypeId::INT64, {TypeId::INT64});
    auto countOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue count(ctx, countOp, SQLType::bigint());
    SQLValue one = SQLValue::makeConst(ctx, 1, SQLType::bigint());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, count, one);
    ctx.builder().emitRet(TypeId::INT64, result.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {0})) == 1);
    REQUIRE(static_cast<int64_t>(run(program, fn, {99})) == 100);
}

// ── BIGINT to INTEGER cast ──────────────────────────────────────────────────

TEST_CASE("SQLValue: cast BIGINT to INTEGER (truncate)", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_bigint_int", TypeId::INT32, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::bigint());
    auto casted = a.castTo(ctx, SQLType::integer());
    REQUIRE(casted.type().id == SQLTypeId::INTEGER);
    ctx.builder().emitRet(TypeId::INT32, casted.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int32_t>(run(program, fn, {42})) == 42);
}

// ── Same type cast is identity ──────────────────────────────────────────────

TEST_CASE("SQLValue: cast same type returns identity", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cast_same", TypeId::INT64, {TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);

    SQLValue a(ctx, aOp, SQLType::bigint());
    auto casted = a.castTo(ctx, SQLType::bigint());
    REQUIRE(casted.value() == a.value());
    ctx.builder().emitRet(TypeId::INT64, casted.value());

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {99})) == 99);
}

// ── DATE operations ─────────────────────────────────────────────────────────

TEST_CASE("SQLValue: DATE add integer days", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // DATE + INTEGER → add days (both INT32)
    auto fn = ctx.createFunction("date_add", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
    auto dateOp = ctx.builder().emitFunArg(TypeId::INT32, 0);
    auto daysOp = ctx.builder().emitFunArg(TypeId::INT32, 1);

    SQLValue dateVal(ctx, dateOp, SQLType::date());
    SQLValue daysVal(ctx, daysOp, SQLType::integer());

    auto result = SQLValue::evaluateBinary(ctx, BinaryOp::ADD, dateVal, daysVal);
    ctx.builder().emitRet(TypeId::INT32, result.value());

    auto program = ctx.finalize();
    // date 100 + 30 days = 130
    REQUIRE(static_cast<int32_t>(run(program, fn, {100, 30})) == 130);
}

// ── Multiple comparisons ────────────────────────────────────────────────────

TEST_CASE("SQLValue: all comparison operators", "[sql_values]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("all_cmp", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto aOp = ctx.builder().emitFunArg(TypeId::INT64, 0);
    auto bOp = ctx.builder().emitFunArg(TypeId::INT64, 1);

    SQLValue a(ctx, aOp, SQLType::bigint());
    SQLValue b(ctx, bOp, SQLType::bigint());

    // Pack results: eq | ne<<1 | lt<<2 | le<<3 | gt<<4 | ge<<5
    auto eq = SQLValue::evaluateCompare(ctx, CmpOp::EQ, a, b);
    auto ne = SQLValue::evaluateCompare(ctx, CmpOp::NE, a, b);
    auto lt = SQLValue::evaluateCompare(ctx, CmpOp::LT, a, b);
    auto le = SQLValue::evaluateCompare(ctx, CmpOp::LE, a, b);
    auto gt = SQLValue::evaluateCompare(ctx, CmpOp::GT, a, b);
    auto ge = SQLValue::evaluateCompare(ctx, CmpOp::GE, a, b);

    // Zero-extend bool results to Int64
    auto eqI = ctx.builder().emitZExt(TypeId::INT64, eq.value());
    auto neI = ctx.builder().emitZExt(TypeId::INT64, ne.value());
    auto ltI = ctx.builder().emitZExt(TypeId::INT64, lt.value());
    auto leI = ctx.builder().emitZExt(TypeId::INT64, le.value());
    auto gtI = ctx.builder().emitZExt(TypeId::INT64, gt.value());
    auto geI = ctx.builder().emitZExt(TypeId::INT64, ge.value());

    // Shift and combine
    auto one = ctx.builder().emitConst(TypeId::INT64, 1);
    auto two = ctx.builder().emitConst(TypeId::INT64, 2);
    auto three = ctx.builder().emitConst(TypeId::INT64, 3);
    auto four = ctx.builder().emitConst(TypeId::INT64, 4);
    auto five = ctx.builder().emitConst(TypeId::INT64, 5);

    auto neShift = ctx.builder().emitShl(TypeId::INT64, neI, one);
    auto ltShift = ctx.builder().emitShl(TypeId::INT64, ltI, two);
    auto leShift = ctx.builder().emitShl(TypeId::INT64, leI, three);
    auto gtShift = ctx.builder().emitShl(TypeId::INT64, gtI, four);
    auto geShift = ctx.builder().emitShl(TypeId::INT64, geI, five);

    auto r1 = ctx.builder().emitOr(TypeId::INT64, eqI, neShift);
    auto r2 = ctx.builder().emitOr(TypeId::INT64, r1, ltShift);
    auto r3 = ctx.builder().emitOr(TypeId::INT64, r2, leShift);
    auto r4 = ctx.builder().emitOr(TypeId::INT64, r3, gtShift);
    auto r5 = ctx.builder().emitOr(TypeId::INT64, r4, geShift);

    ctx.builder().emitRet(TypeId::INT64, r5);

    auto program = ctx.finalize();

    // a=3, b=5: eq=0, ne=1, lt=1, le=1, gt=0, ge=0
    // result = 0 | 2 | 4 | 8 = 14
    REQUIRE(run(program, fn, {3, 5}) == 14);

    // a=5, b=3: eq=0, ne=1, lt=0, le=0, gt=1, ge=1
    // result = 0 | 2 | 0 | 0 | 16 | 32 = 50
    REQUIRE(run(program, fn, {5, 3}) == 50);

    // a=3, b=3: eq=1, ne=0, lt=0, le=1, gt=0, ge=1
    // result = 1 | 0 | 0 | 8 | 0 | 32 = 41
    REQUIRE(run(program, fn, {3, 3}) == 41);
}
