#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "codegen/api/codegen_context.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::codegen;
using namespace umbra::ir;
using umbra::interpreter::Interpreter;

// ── Helper ──────────────────────────────────────────────────────────────────

/// Execute a single-function program and return the result.
static uint64_t run(Program& program, FunctionId func, std::vector<uint64_t> args = {}) {
    Interpreter interp;
    return interp.execute(program, func, std::span<const uint64_t>(args));
}

// ── Typed constants ─────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: constant creation and return", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("test_const", TypeId::INT64, {});
    auto val = ctx.constInt64(42);
    ctx.emitRet(val);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn)) == 42);
}

TEST_CASE("CodegenAPI: bool constant", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("test_bool", TypeId::BOOL, {});
    auto val = ctx.constBool(true);
    ctx.emitRet(val);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn) == 1);
}

TEST_CASE("CodegenAPI: int32 constant", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("test_i32", TypeId::INT32, {});
    auto val = ctx.constInt32(-7);
    ctx.emitRet(val);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int32_t>(run(program, fn)) == -7);
}

TEST_CASE("CodegenAPI: uint64 constant", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("test_u64", TypeId::UINT64, {});
    auto val = ctx.constUInt64(999999ULL);
    ctx.emitRet(val);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn) == 999999ULL);
}

// ── Operator overloads ──────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Int64 arithmetic operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("arith64", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);
    auto sum = a + b;
    auto diff = a - b;
    auto prod = sum * diff;  // (a+b)*(a-b)
    ctx.emitRet(prod);

    auto program = ctx.finalize();
    // (5+3)*(5-3) = 16
    REQUIRE(static_cast<int64_t>(run(program, fn, {5, 3})) == 16);
    // (10+2)*(10-2) = 96
    REQUIRE(static_cast<int64_t>(run(program, fn, {10, 2})) == 96);
}

TEST_CASE("CodegenAPI: Int64 division", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("div64", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);
    auto result = a / b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {17, 3})) == 5);
    REQUIRE(static_cast<int64_t>(run(program, fn, {100, 10})) == 10);
}

TEST_CASE("CodegenAPI: Int32 arithmetic operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("arith32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto b = ctx.getArg<Int32>(1);
    auto result = a + b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int32_t>(run(program, fn, {100, 200})) == 300);
}

TEST_CASE("CodegenAPI: UInt64 arithmetic and XOR", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("xor_test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});
    auto a = ctx.getArg<UInt64>(0);
    auto b = ctx.getArg<UInt64>(1);
    auto result = a ^ b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {0xFF00, 0x00FF}) == 0xFFFF);
    REQUIRE(run(program, fn, {123, 123}) == 0);
}

// ── Comparison operators ────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Int64 comparison operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Test a < b
    auto fn = ctx.createFunction("cmp_lt", TypeId::BOOL, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);
    auto result = a < b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {3, 5}) == 1);
    REQUIRE(run(program, fn, {5, 3}) == 0);
    REQUIRE(run(program, fn, {3, 3}) == 0);
}

TEST_CASE("CodegenAPI: Int64 comparison le/ge", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cmp_le", TypeId::BOOL, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);
    auto result = a <= b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {3, 5}) == 1);
    REQUIRE(run(program, fn, {5, 3}) == 0);
    REQUIRE(run(program, fn, {3, 3}) == 1);
}

TEST_CASE("CodegenAPI: Int32 comparison", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("cmp32_eq", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto b = ctx.getArg<Int32>(1);
    auto result = a == b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {42, 42}) == 1);
    REQUIRE(run(program, fn, {42, 43}) == 0);
}

// ── Logical operations ──────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: logical NOT", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("lnot_test", TypeId::BOOL, {TypeId::BOOL});
    auto a = ctx.getArg<Bool>(0);
    auto result = ctx.lnot(a);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {1}) == 0);
    REQUIRE(run(program, fn, {0}) == 1);
}

TEST_CASE("CodegenAPI: logical AND/OR", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Test: a AND (NOT b)
    auto fn = ctx.createFunction("land_test", TypeId::BOOL, {TypeId::BOOL, TypeId::BOOL});
    auto a = ctx.getArg<Bool>(0);
    auto b = ctx.getArg<Bool>(1);
    auto notB = ctx.lnot(b);
    auto result = ctx.land(a, notB);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {1, 0}) == 1);  // true AND (NOT false) = true
    REQUIRE(run(program, fn, {1, 1}) == 0);  // true AND (NOT true) = false
    REQUIRE(run(program, fn, {0, 0}) == 0);  // false AND (NOT false) = false
}

TEST_CASE("CodegenAPI: logical OR", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("lor_test", TypeId::BOOL, {TypeId::BOOL, TypeId::BOOL});
    auto a = ctx.getArg<Bool>(0);
    auto b = ctx.getArg<Bool>(1);
    auto result = ctx.lor(a, b);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {0, 0}) == 0);
    REQUIRE(run(program, fn, {1, 0}) == 1);
    REQUIRE(run(program, fn, {0, 1}) == 1);
    REQUIRE(run(program, fn, {1, 1}) == 1);
}

// ── Select (ternary) ────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: select/ternary", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("select_test", TypeId::INT64,
                                 {TypeId::BOOL, TypeId::INT64, TypeId::INT64});
    auto cond = ctx.getArg<Bool>(0);
    auto a = ctx.getArg<Int64>(1);
    auto b = ctx.getArg<Int64>(2);
    auto result = ctx.select(cond, a, b);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {1, 100, 200})) == 100);
    REQUIRE(static_cast<int64_t>(run(program, fn, {0, 100, 200})) == 200);
}

// ── Cast operations ─────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: sext Int32 to Int64", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("sext_test", TypeId::INT64, {TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto extended = ctx.sext<Int64>(a);
    ctx.emitRet(extended);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {42})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, fn, {static_cast<uint64_t>(-10)})) == -10);
}

TEST_CASE("CodegenAPI: trunc Int64 to Int32", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("trunc_test", TypeId::INT32, {TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto truncated = ctx.trunc<Int32>(a);
    ctx.emitRet(truncated);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int32_t>(run(program, fn, {12345})) == 12345);
}

// ── If-then-else ────────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: IfBuilder with phi", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // if (a < b) return a; else return b;  (i.e., min(a,b))
    auto fn = ctx.createFunction("min_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);

    auto cond = a < b;
    IfBuilder ifBld(ctx, cond);
    auto thenVal = a;
    ifBld.elseBlock();
    auto elseVal = b;
    ifBld.done();
    auto result = ifBld.phi(thenVal, elseVal);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {3, 7})) == 3);
    REQUIRE(static_cast<int64_t>(run(program, fn, {9, 2})) == 2);
    REQUIRE(static_cast<int64_t>(run(program, fn, {5, 5})) == 5);
}

TEST_CASE("CodegenAPI: IfBuilder without else", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // fn(a): if (a > 10) return a; else return 0;
    auto fn = ctx.createFunction("clamp_fn", TypeId::INT64, {TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto ten = ctx.constInt64(10);
    auto zero = ctx.constInt64(0);

    auto cond = a > ten;
    IfBuilder ifBld(ctx, cond);
    auto thenVal = a;
    ifBld.elseBlock();
    auto elseVal = zero;
    ifBld.done();
    auto result = ifBld.phi(thenVal, elseVal);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {15})) == 15);
    REQUIRE(static_cast<int64_t>(run(program, fn, {5})) == 0);
}

// ── Loop ────────────────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: LoopBuilder — sum 1..N", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // sum(n): returns 1 + 2 + ... + n
    auto fn = ctx.createFunction("sum_n", TypeId::INT64, {TypeId::INT64});
    auto n = ctx.getArg<Int64>(0);

    auto initI = ctx.constInt64(1);
    auto initSum = ctx.constInt64(0);

    LoopBuilder loop(ctx, {initI.op(), initSum.op()}, {TypeId::INT64, TypeId::INT64});
    auto loopI = loop.getLoopVar<Int64>(0);
    auto loopSum = loop.getLoopVar<Int64>(1);

    auto newSum = loopI + loopSum;
    auto one = ctx.constInt64(1);
    auto nextI = loopI + one;
    auto continueLoop = nextI <= n;

    loop.done(continueLoop, {nextI.op(), newSum.op()});

    // After loop exits, the final sum is in the exit block.
    // We need the final PHI values — they are the loop vars at exit.
    // Actually, the loop PHI holds the last iterated values, but when the condition
    // fails, we already advanced nextI and newSum. Let's return newSum from the phi.
    // Since the condition check is at the end, the last executed body had the correct sum.
    ctx.emitRet(newSum);

    auto program = ctx.finalize();
    // sum(10) = 55
    REQUIRE(static_cast<int64_t>(run(program, fn, {10})) == 55);
    // sum(1) = 1
    REQUIRE(static_cast<int64_t>(run(program, fn, {1})) == 1);
    // sum(100) = 5050
    REQUIRE(static_cast<int64_t>(run(program, fn, {100})) == 5050);
}

// ── Pointer load/store ──────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Ptr load and store", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // fn(ptr): store 42 to *ptr, then load and return.
    auto fn = ctx.createFunction("ptr_test", TypeId::INT64, {TypeId::PTR});
    auto ptr = ctx.getArg<Ptr<Int64>>(0);

    auto val = ctx.constInt64(42);
    ptr.store(ctx, val);
    auto loaded = ptr.load(ctx);
    ctx.emitRet(loaded);

    auto program = ctx.finalize();

    int64_t storage = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto addr = reinterpret_cast<uint64_t>(&storage);
    REQUIRE(static_cast<int64_t>(run(program, fn, {addr})) == 42);
    REQUIRE(storage == 42);
}

TEST_CASE("CodegenAPI: Ptr GEP (array indexing)", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // fn(basePtr, idx): return basePtr[idx]
    auto fn = ctx.createFunction("gep_test", TypeId::INT64, {TypeId::PTR, TypeId::INT64});
    auto base = ctx.getArg<Ptr<Int64>>(0);
    auto idx = ctx.getArg<Int64>(1);

    auto elemPtr = base.gep(ctx, idx);
    auto loaded = elemPtr.load(ctx);
    ctx.emitRet(loaded);

    auto program = ctx.finalize();

    std::vector<int64_t> arr = {10, 20, 30, 40, 50};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto addr = reinterpret_cast<uint64_t>(arr.data());
    REQUIRE(static_cast<int64_t>(run(program, fn, {addr, 0})) == 10);
    REQUIRE(static_cast<int64_t>(run(program, fn, {addr, 2})) == 30);
    REQUIRE(static_cast<int64_t>(run(program, fn, {addr, 4})) == 50);
}

// ── Host function calls ─────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: host function call", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Register a host function: multiply(a, b) -> a * b
    auto hostId =
        ctx.registerHostFunction("multiply",
                                 [](const uint64_t* args, uint8_t /*count*/) -> uint64_t {
                                     return static_cast<uint64_t>(static_cast<int64_t>(args[0]) *
                                                                  static_cast<int64_t>(args[1]));
                                 },
                                 TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto fn = ctx.createFunction("call_host", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);
    auto result = ctx.call<Int64>(hostId, a, b);
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {6, 7})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, fn, {static_cast<uint64_t>(-3), 5})) == -15);
}

// ── Ptr bitcast ─────────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Ptr bitcast", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // fn(ptr): treat ptr as Ptr<Int32>, load a 32-bit value
    auto fn = ctx.createFunction("bitcast_test", TypeId::INT32, {TypeId::PTR});
    auto rawPtr = ctx.getArg<Ptr<Int64>>(0);
    auto int32Ptr = rawPtr.bitcastTo<Int32>(ctx);
    auto loaded = int32Ptr.load(ctx);
    ctx.emitRet(loaded);

    auto program = ctx.finalize();

    int32_t storage = 99;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto addr = reinterpret_cast<uint64_t>(&storage);
    REQUIRE(static_cast<int32_t>(run(program, fn, {addr})) == 99);
}

// ── Const pointer ───────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: constPtr embedding", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    int64_t value = 12345;

    auto fn = ctx.createFunction("const_ptr_test", TypeId::INT64, {});
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto embeddedPtr = ctx.constPtr<Int64>(reinterpret_cast<void*>(&value));
    auto loaded = embeddedPtr.load(ctx);
    ctx.emitRet(loaded);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn)) == 12345);
}

// ── Int8 constant ───────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Int8 constant", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("test_i8", TypeId::INT8, {});
    auto val = ctx.constInt8(65);  // ASCII 'A'
    ctx.emitRet(val);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int8_t>(run(program, fn)) == 65);
}

// ── Int32 mul/div/sub operators ─────────────────────────────────────────────

TEST_CASE("CodegenAPI: Int32 sub/mul/div operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("i32_sub", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto b = ctx.getArg<Int32>(1);
    auto sub = a - b;
    auto mul = a * b;
    auto div = a / b;
    // Return sub + mul - div as a combined test
    auto result = sub + mul - div;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    // a=10, b=3: sub=7, mul=30, div=3, result = 7 + 30 - 3 = 34
    REQUIRE(static_cast<int32_t>(run(program, fn, {10, 3})) == 34);
}

TEST_CASE("CodegenAPI: Int32 comparison ne/lt/le/gt/ge", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("i32_cmp_ne", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto b = ctx.getArg<Int32>(1);
    auto result = a != b;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    REQUIRE(run(program, fn, {1, 2}) == 1);
    REQUIRE(run(program, fn, {3, 3}) == 0);
}

TEST_CASE("CodegenAPI: Int32 comparison lt/le/gt/ge", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Build a function that returns packed comparison results
    auto fn = ctx.createFunction("i32_cmp_all", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
    auto a = ctx.getArg<Int32>(0);
    auto b = ctx.getArg<Int32>(1);

    auto lt = a < b;
    auto le = a <= b;
    auto gt = a > b;
    auto ge = a >= b;

    // Combine results: lt + le*2 + gt*4 + ge*8
    auto ltI = ctx.zext<Int32>(lt);
    auto leI = ctx.zext<Int32>(le);
    auto gtI = ctx.zext<Int32>(gt);
    auto geI = ctx.zext<Int32>(ge);

    auto two = ctx.constInt32(2);
    auto four = ctx.constInt32(4);
    auto eight = ctx.constInt32(8);

    auto result = ltI + leI * two + gtI * four + geI * eight;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    // a=1, b=2: lt=1, le=1, gt=0, ge=0 → 1 + 2 + 0 + 0 = 3
    REQUIRE(static_cast<int32_t>(run(program, fn, {1, 2})) == 3);
    // a=3, b=3: lt=0, le=1, gt=0, ge=1 → 0 + 2 + 0 + 8 = 10
    REQUIRE(static_cast<int32_t>(run(program, fn, {3, 3})) == 10);
}

// ── Int64 eq/ne/ge operators ────────────────────────────────────────────────

TEST_CASE("CodegenAPI: Int64 comparison eq/ne/ge", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("i64_eq_ne_ge", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = ctx.getArg<Int64>(0);
    auto b = ctx.getArg<Int64>(1);

    auto eq = a == b;
    auto ne = a != b;
    auto ge = a >= b;

    auto eqI = ctx.zext<Int64>(eq);
    auto neI = ctx.zext<Int64>(ne);
    auto geI = ctx.zext<Int64>(ge);

    auto two = ctx.constInt64(2);
    auto four = ctx.constInt64(4);
    auto result = eqI + neI * two + geI * four;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    // a=5, b=5: eq=1, ne=0, ge=1 → 1 + 0 + 4 = 5
    REQUIRE(static_cast<int64_t>(run(program, fn, {5, 5})) == 5);
    // a=3, b=5: eq=0, ne=1, ge=0 → 0 + 2 + 0 = 2
    REQUIRE(static_cast<int64_t>(run(program, fn, {3, 5})) == 2);
}

// ── UInt64 arithmetic operators ─────────────────────────────────────────────

TEST_CASE("CodegenAPI: UInt64 add/sub/mul/div operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("u64_arith", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});
    auto a = ctx.getArg<UInt64>(0);
    auto b = ctx.getArg<UInt64>(1);

    auto sum = a + b;
    auto diff = a - b;
    auto prod = sum * diff;
    auto quot = prod / b;
    ctx.emitRet(quot);

    auto program = ctx.finalize();
    // a=10, b=3: sum=13, diff=7, prod=91, quot=91/3=30
    REQUIRE(run(program, fn, {10, 3}) == 30);
}

TEST_CASE("CodegenAPI: UInt64 comparison operators", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("u64_cmp", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});
    auto a = ctx.getArg<UInt64>(0);
    auto b = ctx.getArg<UInt64>(1);

    auto eq = a == b;
    auto ne = a != b;
    auto lt = a < b;
    auto le = a <= b;
    auto gt = a > b;
    auto ge = a >= b;

    auto eqI = ctx.zext<UInt64>(eq);
    auto neI = ctx.zext<UInt64>(ne);
    auto ltI = ctx.zext<UInt64>(lt);
    auto leI = ctx.zext<UInt64>(le);
    auto gtI = ctx.zext<UInt64>(gt);
    auto geI = ctx.zext<UInt64>(ge);

    auto two = ctx.constUInt64(2);
    auto four = ctx.constUInt64(4);
    auto eight = ctx.constUInt64(8);
    auto sixteen = ctx.constUInt64(16);
    auto thirtyTwo = ctx.constUInt64(32);

    auto result = eqI + neI * two + ltI * four + leI * eight + gtI * sixteen + geI * thirtyTwo;
    ctx.emitRet(result);

    auto program = ctx.finalize();
    // a=3, b=5: eq=0, ne=1, lt=1, le=1, gt=0, ge=0 → 0+2+4+8 = 14
    REQUIRE(run(program, fn, {3, 5}) == 14);
    // a=5, b=5: eq=1, ne=0, lt=0, le=1, gt=0, ge=1 → 1+0+0+8+0+32 = 41
    REQUIRE(run(program, fn, {5, 5}) == 41);
}

// ── emitRetVoid ─────────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: void function", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    // Create a void function that writes a value to a pointer
    auto fn = ctx.createFunction("void_fn", TypeId::VOID, {TypeId::PTR});
    auto ptr = ctx.getArg<Ptr<Int64>>(0);
    auto val = ctx.constInt64(999);
    ptr.store(ctx, val);
    ctx.emitRetVoid();

    auto program = ctx.finalize();
    int64_t storage = 0;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto addr = reinterpret_cast<uint64_t>(&storage);
    run(program, fn, {addr});
    REQUIRE(storage == 999);
}

// ── Zero extend ─────────────────────────────────────────────────────────────

TEST_CASE("CodegenAPI: zext Bool to Int64", "[codegen_api]") {
    CodegenContext ctx;
    setActiveContext(&ctx);

    auto fn = ctx.createFunction("zext_test", TypeId::INT64, {TypeId::BOOL});
    auto a = ctx.getArg<Bool>(0);
    auto extended = ctx.zext<Int64>(a);
    ctx.emitRet(extended);

    auto program = ctx.finalize();
    REQUIRE(static_cast<int64_t>(run(program, fn, {1})) == 1);
    REQUIRE(static_cast<int64_t>(run(program, fn, {0})) == 0);
}
