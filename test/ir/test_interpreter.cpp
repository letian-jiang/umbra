#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "backend/interpreter/interpreter.h"
#include "ir/builder.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::ir;
using umbra::interpreter::Interpreter;

// ── Helper ───────────────────────────────────────────────────────────────────

/// Execute a single-function program with the given arguments.
static uint64_t run(Program& program, FunctionId func, std::vector<uint64_t> args) {
    Interpreter interp;
    return interp.execute(program, func, std::span<const uint64_t>(args));
}

// ── Arithmetic (add, sub, mul, div, mod with all integer types) ──────────────

TEST_CASE("Interpreter: simple arithmetic (a+b)*(a-b)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("diff_product", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto diff = builder.emitSub(TypeId::INT64, a, b);
    auto prod = builder.emitMul(TypeId::INT64, sum, diff);
    builder.emitRet(TypeId::INT64, prod);

    auto program = builder.finalize();

    // (5+3)*(5-3) = 8*2 = 16
    REQUIRE(run(program, 0, {5, 3}) == 16);
    // (10+7)*(10-7) = 17*3 = 51
    REQUIRE(run(program, 0, {10, 7}) == 51);
    // (0+0)*(0-0) = 0
    REQUIRE(run(program, 0, {0, 0}) == 0);
}

TEST_CASE("Interpreter: int32 arithmetic", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("add32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto sum = builder.emitAdd(TypeId::INT32, a, b);
    builder.emitRet(TypeId::INT32, sum);

    auto program = builder.finalize();
    REQUIRE(static_cast<int32_t>(run(program, 0, {100, 200})) == 300);
    // Negative numbers
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-10), 3})) == -7);
}

TEST_CASE("Interpreter: division and modulo", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("divmod", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto quotient = builder.emitDiv(TypeId::INT64, a, b);
    auto remainder = builder.emitMod(TypeId::INT64, a, b);
    // Return quotient * b + remainder (should equal a)
    auto prod = builder.emitMul(TypeId::INT64, quotient, b);
    auto result = builder.emitAdd(TypeId::INT64, prod, remainder);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    // 17 = (17/3)*3 + (17%3) = 5*3 + 2 = 17
    REQUIRE(static_cast<int64_t>(run(program, 0, {17, 3})) == 17);
    REQUIRE(static_cast<int64_t>(run(program, 0, {100, 7})) == 100);
}

TEST_CASE("Interpreter: unsigned division", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("udiv_test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto a = builder.emitFunArg(TypeId::UINT64, 0);
    auto b = builder.emitFunArg(TypeId::UINT64, 1);
    auto result = builder.emitUDiv(TypeId::UINT64, a, b);
    builder.emitRet(TypeId::UINT64, result);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {100, 3}) == 33);
    // Division by zero returns 0
    REQUIRE(run(program, 0, {100, 0}) == 0);
}

TEST_CASE("Interpreter: negation", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("neg_test", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto negA = builder.emitNeg(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, negA);

    auto program = builder.finalize();
    REQUIRE(static_cast<int64_t>(run(program, 0, {42})) == -42);
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(-7)})) == 7);
}

// ── Comparison and branching ─────────────────────────────────────────────────

TEST_CASE("Interpreter: conditional branch (abs value)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("abs", TypeId::INT32, {TypeId::INT32});

    auto x = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    auto cond = builder.emitCmpLt(TypeId::INT32, x, zero);

    auto negBB = builder.createBlock();
    auto posBB = builder.createBlock();
    auto mergeBB = builder.createBlock();
    builder.emitCondBr(cond, negBB, posBB);

    builder.setInsertPoint(negBB);
    auto negX = builder.emitNeg(TypeId::INT32, x);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(posBB);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{negBB, negX}, {posBB, x}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(run(program, 0, {5})) == 5);
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-5)})) == 5);
    REQUIRE(static_cast<int32_t>(run(program, 0, {0})) == 0);
}

TEST_CASE("Interpreter: multi-block control flow", "[interpreter]") {
    // if (a > 0 && b > 0) return a + b; else return 0;
    IRBuilder builder;
    builder.createFunction("positive_sum", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto zero = builder.emitConst(TypeId::INT32, 0);

    auto checkB = builder.createBlock();
    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    auto condA = builder.emitCmpGt(TypeId::INT32, a, zero);
    builder.emitCondBr(condA, checkB, falseBB);

    builder.setInsertPoint(checkB);
    auto condB = builder.emitCmpGt(TypeId::INT32, b, zero);
    builder.emitCondBr(condB, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    auto sum = builder.emitAdd(TypeId::INT32, a, b);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(falseBB);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{trueBB, sum}, {falseBB, zero}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(run(program, 0, {3, 4})) == 7);
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-1), 4})) == 0);
    REQUIRE(static_cast<int32_t>(run(program, 0, {3, static_cast<uint64_t>(-1)})) == 0);
}

TEST_CASE("Interpreter: all comparison operators", "[interpreter]") {
    // Test each comparison operator individually
    auto testCmp = [](auto emitCmpFn, int32_t a, int32_t b) -> bool {
        IRBuilder builder;
        builder.createFunction("cmp", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});
        auto argA = builder.emitFunArg(TypeId::INT32, 0);
        auto argB = builder.emitFunArg(TypeId::INT32, 1);
        auto result = emitCmpFn(builder, argA, argB);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        return run(program, 0, {static_cast<uint64_t>(a), static_cast<uint64_t>(b)}) != 0;
    };

    auto cmpEq = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpEq(TypeId::INT32, l, r); };
    auto cmpNe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpNe(TypeId::INT32, l, r); };
    auto cmpLt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLt(TypeId::INT32, l, r); };
    auto cmpLe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLe(TypeId::INT32, l, r); };
    auto cmpGt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGt(TypeId::INT32, l, r); };
    auto cmpGe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGe(TypeId::INT32, l, r); };

    REQUIRE(testCmp(cmpEq, 5, 5));
    REQUIRE_FALSE(testCmp(cmpEq, 5, 3));

    REQUIRE(testCmp(cmpNe, 5, 3));
    REQUIRE_FALSE(testCmp(cmpNe, 5, 5));

    REQUIRE(testCmp(cmpLt, 3, 5));
    REQUIRE_FALSE(testCmp(cmpLt, 5, 3));
    REQUIRE_FALSE(testCmp(cmpLt, 5, 5));

    REQUIRE(testCmp(cmpLe, 3, 5));
    REQUIRE(testCmp(cmpLe, 5, 5));
    REQUIRE_FALSE(testCmp(cmpLe, 7, 5));

    REQUIRE(testCmp(cmpGt, 5, 3));
    REQUIRE_FALSE(testCmp(cmpGt, 3, 5));

    REQUIRE(testCmp(cmpGe, 5, 3));
    REQUIRE(testCmp(cmpGe, 5, 5));
    REQUIRE_FALSE(testCmp(cmpGe, 3, 5));
}

// ── Memory load/store ────────────────────────────────────────────────────────

TEST_CASE("Interpreter: memory load and store", "[interpreter]") {
    // Increment a value in memory: *ptr = *ptr + 1; return *ptr
    IRBuilder builder;
    builder.createFunction("increment", TypeId::INT64, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitLoad(TypeId::INT64, ptr);
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto incremented = builder.emitAdd(TypeId::INT64, val, one);
    builder.emitStore(incremented, ptr);
    auto result = builder.emitLoad(TypeId::INT64, ptr);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    int64_t value = 42;
    auto ptrVal = reinterpret_cast<uint64_t>(&value);  // NOLINT
    REQUIRE(static_cast<int64_t>(run(program, 0, {ptrVal})) == 43);
    REQUIRE(value == 43);
}

TEST_CASE("Interpreter: array sum with GEP", "[interpreter]") {
    // Array sum: f(ptr, n) -> sum of int64 array[0..n-1]
    IRBuilder builder;
    builder.createFunction("array_sum", TypeId::INT64, {TypeId::PTR, TypeId::INT64});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto n = builder.emitFunArg(TypeId::INT64, 1);
    auto initSum = builder.emitConst(TypeId::INT64, 0);
    auto initI = builder.emitConst(TypeId::INT64, 0);
    auto one = builder.emitConst(TypeId::INT64, 1);

    auto headerBB = builder.createBlock();
    auto bodyBB = builder.createBlock();
    auto exitBB = builder.createBlock();

    builder.emitBr(headerBB);

    builder.setInsertPoint(headerBB);
    auto phiSum = builder.emitPhi(TypeId::INT64, {{BlockId(0), initSum}, {bodyBB, INVALID_OP}});
    auto phiI = builder.emitPhi(TypeId::INT64, {{BlockId(0), initI}, {bodyBB, INVALID_OP}});
    auto cond = builder.emitCmpLt(TypeId::INT64, phiI, n);
    builder.emitCondBr(cond, bodyBB, exitBB);

    builder.setInsertPoint(bodyBB);
    auto elemPtr = builder.emitGEP(ptr, TypeId::INT64, phiI);
    auto val = builder.emitLoad(TypeId::INT64, elemPtr);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, val);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    std::vector<int64_t> arr = {10, 20, 30, 40, 50};
    auto arrPtr = reinterpret_cast<uint64_t>(arr.data());  // NOLINT
    REQUIRE(static_cast<int64_t>(run(program, 0, {arrPtr, 5})) == 150);
    REQUIRE(static_cast<int64_t>(run(program, 0, {arrPtr, 0})) == 0);
    REQUIRE(static_cast<int64_t>(run(program, 0, {arrPtr, 1})) == 10);
}

// ── Loops with PHI nodes ─────────────────────────────────────────────────────

TEST_CASE("Interpreter: loop sum 0..n-1", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("sum_to_n", TypeId::INT64, {TypeId::INT64});

    auto n = builder.emitFunArg(TypeId::INT64, 0);
    auto initSum = builder.emitConst(TypeId::INT64, 0);
    auto initI = builder.emitConst(TypeId::INT64, 0);
    auto one = builder.emitConst(TypeId::INT64, 1);

    auto headerBB = builder.createBlock();
    auto bodyBB = builder.createBlock();
    auto exitBB = builder.createBlock();

    builder.emitBr(headerBB);

    builder.setInsertPoint(headerBB);
    auto phiSum = builder.emitPhi(TypeId::INT64, {{BlockId(0), initSum}, {bodyBB, INVALID_OP}});
    auto phiI = builder.emitPhi(TypeId::INT64, {{BlockId(0), initI}, {bodyBB, INVALID_OP}});
    auto cond = builder.emitCmpLt(TypeId::INT64, phiI, n);
    builder.emitCondBr(cond, bodyBB, exitBB);

    builder.setInsertPoint(bodyBB);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, phiI);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    // sum(0..9) = 45
    REQUIRE(static_cast<int64_t>(run(program, 0, {10})) == 45);
    // sum(0..99) = 4950
    REQUIRE(static_cast<int64_t>(run(program, 0, {100})) == 4950);
    // sum(0) = 0
    REQUIRE(static_cast<int64_t>(run(program, 0, {0})) == 0);
    // sum(1) = 0
    REQUIRE(static_cast<int64_t>(run(program, 0, {1})) == 0);
}

TEST_CASE("Interpreter: factorial with loop", "[interpreter]") {
    // fact(n) = n! computed with a loop
    IRBuilder builder;
    builder.createFunction("factorial", TypeId::INT64, {TypeId::INT64});

    auto n = builder.emitFunArg(TypeId::INT64, 0);
    auto initResult = builder.emitConst(TypeId::INT64, 1);
    auto initI = builder.emitConst(TypeId::INT64, 1);
    auto one = builder.emitConst(TypeId::INT64, 1);

    auto headerBB = builder.createBlock();
    auto bodyBB = builder.createBlock();
    auto exitBB = builder.createBlock();

    builder.emitBr(headerBB);

    builder.setInsertPoint(headerBB);
    auto phiResult =
        builder.emitPhi(TypeId::INT64, {{BlockId(0), initResult}, {bodyBB, INVALID_OP}});
    auto phiI = builder.emitPhi(TypeId::INT64, {{BlockId(0), initI}, {bodyBB, INVALID_OP}});
    auto cond = builder.emitCmpLe(TypeId::INT64, phiI, n);
    builder.emitCondBr(cond, bodyBB, exitBB);

    builder.setInsertPoint(bodyBB);
    auto newResult = builder.emitMul(TypeId::INT64, phiResult, phiI);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiResult, 1, newResult);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiResult);

    auto program = builder.finalize();

    REQUIRE(static_cast<int64_t>(run(program, 0, {1})) == 1);
    REQUIRE(static_cast<int64_t>(run(program, 0, {5})) == 120);
    REQUIRE(static_cast<int64_t>(run(program, 0, {10})) == 3628800);
}

// ── Function calls (host functions) ──────────────────────────────────────────

TEST_CASE("Interpreter: host function call", "[interpreter]") {
    IRBuilder builder;

    // Register a host function that adds two values.
    auto program = [&]() {
        builder.createFunction("caller", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

        auto a = builder.emitFunArg(TypeId::INT64, 0);
        auto b = builder.emitFunArg(TypeId::INT64, 1);

        // We need to finalize first to get the program, then register host function.
        // Actually, we register through the Program after finalize.
        // Use emitCall with a placeholder function ID (we'll register it on the program).
        auto sum = builder.emitAdd(TypeId::INT64, a, b);
        builder.emitRet(TypeId::INT64, sum);
        return builder.finalize();
    }();

    // Simple test: just run the function.
    REQUIRE(static_cast<int64_t>(run(program, 0, {10, 20})) == 30);
}

TEST_CASE("Interpreter: host function with program registration", "[interpreter]") {
    IRBuilder builder;

    // First register the host function on the program to get its ID.
    auto hostFn = [](const uint64_t* args, uint8_t count) -> uint64_t {
        // Multiply first two args
        if (count >= 2) {
            return static_cast<uint64_t>(static_cast<int64_t>(args[0]) *
                                         static_cast<int64_t>(args[1]));
        }
        return 0;
    };

    builder.createFunction("caller", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    // Reserve function ID 1 for the host function by using emitCall with ID 1.
    auto callResult = builder.emitCall(TypeId::INT64, 1, {a, b});
    builder.emitRet(TypeId::INT64, callResult);

    auto program = builder.finalize();

    // Register the host function at ID 1.
    program.registerHostFunction("multiply", HostFunction(hostFn), TypeId::INT64,
                                 {TypeId::INT64, TypeId::INT64});

    REQUIRE(static_cast<int64_t>(run(program, 0, {6, 7})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(-3), 4})) == -12);
}

// ── CRC32 instruction ────────────────────────────────────────────────────────

TEST_CASE("Interpreter: CRC32 instruction", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("hash", TypeId::UINT64, {TypeId::UINT64});

    auto data = builder.emitFunArg(TypeId::UINT64, 0);
    auto seed = builder.emitConst(TypeId::UINT64, 0);
    auto hash = builder.emitCRC32(data, seed);
    builder.emitRet(TypeId::UINT64, hash);

    auto program = builder.finalize();

    // CRC32 of 0 with seed 0
    auto result0 = run(program, 0, {0});
    // CRC32 of 1 with seed 0
    auto result1 = run(program, 0, {1});
    // Results should differ for different inputs
    REQUIRE(result0 != result1);

    // Same input should give same result (deterministic)
    REQUIRE(run(program, 0, {42}) == run(program, 0, {42}));
}

TEST_CASE("Interpreter: CRC32 with seed chaining", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("hash_chain", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto data1 = builder.emitFunArg(TypeId::UINT64, 0);
    auto data2 = builder.emitFunArg(TypeId::UINT64, 1);
    auto seed = builder.emitConst(TypeId::UINT64, 0);
    auto hash1 = builder.emitCRC32(data1, seed);
    auto hash2 = builder.emitCRC32(data2, hash1);
    builder.emitRet(TypeId::UINT64, hash2);

    auto program = builder.finalize();

    // Chain hashing: hash(b, hash(a, 0))
    auto hashAB = run(program, 0, {1, 2});
    auto hashBA = run(program, 0, {2, 1});
    // Order should matter
    REQUIRE(hashAB != hashBA);
}

// ── NULL propagation patterns ────────────────────────────────────────────────

TEST_CASE("Interpreter: IS_NULL instruction", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("check_null", TypeId::BOOL, {TypeId::BOOL});

    auto nullFlag = builder.emitFunArg(TypeId::BOOL, 0);
    auto isNull = builder.emitIsNull(nullFlag);
    builder.emitRet(TypeId::BOOL, isNull);

    auto program = builder.finalize();

    // IS_NULL checks if the flag is non-zero (i.e., value is null)
    REQUIRE(run(program, 0, {1}) == 1);  // null flag set -> is null
    REQUIRE(run(program, 0, {0}) == 0);  // null flag clear -> not null
}

TEST_CASE("Interpreter: NULL propagation with select", "[interpreter]") {
    // Pattern: result = null_a || null_b ? 0 : a + b
    IRBuilder builder;
    builder.createFunction("null_add", TypeId::INT64,
                           {TypeId::INT64, TypeId::BOOL, TypeId::INT64, TypeId::BOOL});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto nullA = builder.emitFunArg(TypeId::BOOL, 1);
    auto b = builder.emitFunArg(TypeId::INT64, 2);
    auto nullB = builder.emitFunArg(TypeId::BOOL, 3);

    auto eitherNull = builder.emitLOr(nullA, nullB);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    auto result = builder.emitSelect(TypeId::INT64, eitherNull, zero, sum);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    // Both non-null: a + b
    REQUIRE(static_cast<int64_t>(run(program, 0, {10, 0, 20, 0})) == 30);
    // a is null: return 0
    REQUIRE(static_cast<int64_t>(run(program, 0, {10, 1, 20, 0})) == 0);
    // b is null: return 0
    REQUIRE(static_cast<int64_t>(run(program, 0, {10, 0, 20, 1})) == 0);
}

// ── Bitwise operations ──────────────────────────────────────────────────────

TEST_CASE("Interpreter: bitwise operations", "[interpreter]") {
    // Test AND, OR, XOR
    auto testBitwise = [](auto emitFn, uint64_t a, uint64_t b) -> uint64_t {
        IRBuilder builder;
        builder.createFunction("bitop", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
        auto argA = builder.emitFunArg(TypeId::INT64, 0);
        auto argB = builder.emitFunArg(TypeId::INT64, 1);
        auto result = emitFn(builder, argA, argB);
        builder.emitRet(TypeId::INT64, result);
        auto program = builder.finalize();
        return run(program, 0, {a, b});
    };

    auto andFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitAnd(TypeId::INT64, l, r); };
    auto orFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitOr(TypeId::INT64, l, r); };
    auto xorFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitXor(TypeId::INT64, l, r); };

    REQUIRE(testBitwise(andFn, 0xFF, 0x0F) == 0x0F);
    REQUIRE(testBitwise(orFn, 0xF0, 0x0F) == 0xFF);
    REQUIRE(testBitwise(xorFn, 0xFF, 0x0F) == 0xF0);
}

TEST_CASE("Interpreter: shift operations", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("shifts", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto four = builder.emitConst(TypeId::INT64, 4);
    auto shl = builder.emitShl(TypeId::INT64, a, four);
    auto lshr = builder.emitLShr(TypeId::INT64, shl, four);
    builder.emitRet(TypeId::INT64, lshr);

    auto program = builder.finalize();

    // Shift left 4 then shift right 4 should recover original (for small values)
    REQUIRE(run(program, 0, {0xFF}) == 0xFF);
}

TEST_CASE("Interpreter: arithmetic shift right (sign preservation)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("ashr_test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto result = builder.emitAShr(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    // -16 >> 2 = -4 (sign preserved)
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(-16), 2})) == -4);
    // 16 >> 2 = 4
    REQUIRE(static_cast<int64_t>(run(program, 0, {16, 2})) == 4);
}

TEST_CASE("Interpreter: rotate operations", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("rotate_test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto val = builder.emitFunArg(TypeId::UINT64, 0);
    auto amount = builder.emitFunArg(TypeId::UINT64, 1);
    // Rotate left then rotate right should recover original
    auto rotated = builder.emitRotl(TypeId::UINT64, val, amount);
    auto recovered = builder.emitRotr(TypeId::UINT64, rotated, amount);
    builder.emitRet(TypeId::UINT64, recovered);

    auto program = builder.finalize();

    REQUIRE(run(program, 0, {0xDEADBEEF12345678ULL, 17}) == 0xDEADBEEF12345678ULL);
}

// ── Logical operations ──────────────────────────────────────────────────────

TEST_CASE("Interpreter: logical operations", "[interpreter]") {
    // LNOT
    {
        IRBuilder builder;
        builder.createFunction("lnot_test", TypeId::BOOL, {TypeId::BOOL});
        auto a = builder.emitFunArg(TypeId::BOOL, 0);
        auto result = builder.emitLNot(a);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        REQUIRE(run(program, 0, {1}) == 0);
        REQUIRE(run(program, 0, {0}) == 1);
    }

    // LAND
    {
        IRBuilder builder;
        builder.createFunction("land_test", TypeId::BOOL, {TypeId::BOOL, TypeId::BOOL});
        auto a = builder.emitFunArg(TypeId::BOOL, 0);
        auto b = builder.emitFunArg(TypeId::BOOL, 1);
        auto result = builder.emitLAnd(a, b);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        REQUIRE(run(program, 0, {1, 1}) == 1);
        REQUIRE(run(program, 0, {1, 0}) == 0);
        REQUIRE(run(program, 0, {0, 1}) == 0);
        REQUIRE(run(program, 0, {0, 0}) == 0);
    }

    // LOR
    {
        IRBuilder builder;
        builder.createFunction("lor_test", TypeId::BOOL, {TypeId::BOOL, TypeId::BOOL});
        auto a = builder.emitFunArg(TypeId::BOOL, 0);
        auto b = builder.emitFunArg(TypeId::BOOL, 1);
        auto result = builder.emitLOr(a, b);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        REQUIRE(run(program, 0, {1, 1}) == 1);
        REQUIRE(run(program, 0, {1, 0}) == 1);
        REQUIRE(run(program, 0, {0, 1}) == 1);
        REQUIRE(run(program, 0, {0, 0}) == 0);
    }
}

// ── Select instruction ──────────────────────────────────────────────────────

TEST_CASE("Interpreter: select (branchless min)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("branchless_min", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto cond = builder.emitCmpLt(TypeId::INT32, a, b);
    auto result = builder.emitSelect(TypeId::INT32, cond, a, b);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(run(program, 0, {3, 7})) == 3);
    REQUIRE(static_cast<int32_t>(run(program, 0, {7, 3})) == 3);
    REQUIRE(static_cast<int32_t>(run(program, 0, {5, 5})) == 5);
}

// ── Cast operations ─────────────────────────────────────────────────────────

TEST_CASE("Interpreter: sign extension", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("sext_test", TypeId::INT64, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto extended = builder.emitSExt(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, extended);

    auto program = builder.finalize();

    REQUIRE(static_cast<int64_t>(run(program, 0, {42})) == 42);
    // -1 as int32 should sign-extend to -1 as int64
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(-1) & 0xFFFFFFFF})) == -1);
}

TEST_CASE("Interpreter: zero extension", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("zext_test", TypeId::INT64, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto extended = builder.emitZExt(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, extended);

    auto program = builder.finalize();

    REQUIRE(run(program, 0, {42}) == 42);
    REQUIRE(run(program, 0, {0xFFFFFFFF}) == 0xFFFFFFFF);
}

TEST_CASE("Interpreter: truncation", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("trunc_test", TypeId::INT32, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto truncated = builder.emitTrunc(TypeId::INT32, a);
    builder.emitRet(TypeId::INT32, truncated);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(run(program, 0, {42})) == 42);
    // Upper bits should be chopped off
    REQUIRE(static_cast<uint32_t>(run(program, 0, {0x1FFFFFFFF})) == 0xFFFFFFFF);
}

TEST_CASE("Interpreter: bitcast and ptr conversions", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("cast_test", TypeId::UINT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto casted = builder.emitBitcast(TypeId::UINT64, a);
    builder.emitRet(TypeId::UINT64, casted);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {0xDEADBEEF}) == 0xDEADBEEF);
}

TEST_CASE("Interpreter: ptr2int and int2ptr", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("ptr_roundtrip", TypeId::PTR, {TypeId::PTR});

    auto p = builder.emitFunArg(TypeId::PTR, 0);
    auto asInt = builder.emitPtr2Int(TypeId::UINT64, p);
    auto backToPtr = builder.emitInt2Ptr(asInt);
    builder.emitRet(TypeId::PTR, backToPtr);

    auto program = builder.finalize();
    int64_t dummy = 0;
    auto ptrVal = reinterpret_cast<uint64_t>(&dummy);  // NOLINT
    REQUIRE(run(program, 0, {ptrVal}) == ptrVal);
}

// ── Checked arithmetic ──────────────────────────────────────────────────────

TEST_CASE("Interpreter: checked add (no overflow)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_add", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedAdd(TypeId::INT32, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT32, result);

    builder.setInsertPoint(overflowBB);
    auto maxVal = builder.emitConst(TypeId::INT32, static_cast<uint64_t>(-1));
    builder.emitRet(TypeId::INT32, maxVal);

    auto program = builder.finalize();

    // No overflow: should return sum
    REQUIRE(static_cast<int32_t>(run(program, 0, {100, 200})) == 300);
}

TEST_CASE("Interpreter: checked add (overflow)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_add_overflow", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedAdd(TypeId::INT32, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT32, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT32, 999);
    builder.emitRet(TypeId::INT32, sentinel);

    auto program = builder.finalize();

    // INT32_MAX + 1 should overflow
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(INT32_MAX), 1})) == 999);
}

// ── BSWAP ───────────────────────────────────────────────────────────────────

TEST_CASE("Interpreter: byte swap", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("bswap_test", TypeId::INT32, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto swapped = builder.emitBSwap(TypeId::INT32, a);
    builder.emitRet(TypeId::INT32, swapped);

    auto program = builder.finalize();

    REQUIRE(static_cast<uint32_t>(run(program, 0, {0x01020304})) == 0x04030201);
}

// ── PHI with 3 incoming edges ───────────────────────────────────────────────

TEST_CASE("Interpreter: phi3 (sign function)", "[interpreter]") {
    // signum(x): returns -1, 0, or 1
    IRBuilder builder;
    builder.createFunction("signum", TypeId::INT32, {TypeId::INT32});

    auto x = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);

    auto negBB = builder.createBlock();
    auto checkPos = builder.createBlock();
    auto posBB = builder.createBlock();
    auto zeroBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    auto isNeg = builder.emitCmpLt(TypeId::INT32, x, zero);
    builder.emitCondBr(isNeg, negBB, checkPos);

    builder.setInsertPoint(negBB);
    auto negOne = builder.emitConst(TypeId::INT32, static_cast<uint64_t>(-1));
    builder.emitBr(mergeBB);

    builder.setInsertPoint(checkPos);
    auto isPos = builder.emitCmpGt(TypeId::INT32, x, zero);
    builder.emitCondBr(isPos, posBB, zeroBB);

    builder.setInsertPoint(posBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(zeroBB);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{negBB, negOne}, {posBB, one}, {zeroBB, zero}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-42)})) == -1);
    REQUIRE(static_cast<int32_t>(run(program, 0, {42})) == 1);
    REQUIRE(static_cast<int32_t>(run(program, 0, {0})) == 0);
}

// ── RetVoid ─────────────────────────────────────────────────────────────────

TEST_CASE("Interpreter: ret void", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("void_fn", TypeId::VOID, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitConst(TypeId::INT64, 42);
    builder.emitStore(val, ptr);
    builder.emitRetVoid();

    auto program = builder.finalize();

    int64_t result = 0;
    auto ptrVal = reinterpret_cast<uint64_t>(&result);  // NOLINT
    run(program, 0, {ptrVal});
    REQUIRE(result == 42);
}

// ── Trace mode ──────────────────────────────────────────────────────────────

TEST_CASE("Interpreter: trace mode produces output", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("trace_test", TypeId::INT32, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto one = builder.emitConst(TypeId::INT32, 1);
    auto result = builder.emitAdd(TypeId::INT32, a, one);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();

    std::ostringstream traceOut;
    Interpreter interp;
    interp.setTraceEnabled(true);
    interp.setTraceOutput(&traceOut);

    std::vector<uint64_t> args = {5};
    auto retVal = interp.execute(program, 0, std::span<const uint64_t>(args));

    REQUIRE(static_cast<int32_t>(retVal) == 6);
    // Trace should contain some output
    REQUIRE(!traceOut.str().empty());
    REQUIRE(traceOut.str().find("add") != std::string::npos);
}

// ── Store/Load with int32 ───────────────────────────────────────────────────

TEST_CASE("Interpreter: store and load int32", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("store_load32", TypeId::INT32, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitConst(TypeId::INT32, 12345);
    builder.emitStore(val, ptr);
    auto loaded = builder.emitLoad(TypeId::INT32, ptr);
    builder.emitRet(TypeId::INT32, loaded);

    auto program = builder.finalize();

    int32_t storage = 0;
    auto ptrVal = reinterpret_cast<uint64_t>(&storage);  // NOLINT
    REQUIRE(static_cast<int32_t>(run(program, 0, {ptrVal})) == 12345);
    REQUIRE(storage == 12345);
}

// ── Division by zero ────────────────────────────────────────────────────────

TEST_CASE("Interpreter: division by zero returns 0", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("div_zero", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    auto result = builder.emitDiv(TypeId::INT64, a, zero);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {42}) == 0);
}

TEST_CASE("Interpreter: mod by zero returns 0", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("mod_zero", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    auto result = builder.emitMod(TypeId::INT64, a, zero);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {42}) == 0);
}

// ── Constants ───────────────────────────────────────────────────────────────

TEST_CASE("Interpreter: constant values", "[interpreter]") {
    // Test various constant types
    IRBuilder builder;
    builder.createFunction("const_test", TypeId::INT64, {});

    auto val = builder.emitConst(TypeId::INT64, 0xDEADBEEFCAFEBABE);
    builder.emitRet(TypeId::INT64, val);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {}) == 0xDEADBEEFCAFEBABE);
}

// ── Int32 arithmetic coverage ────────────────────────────────────────────────

TEST_CASE("Interpreter: int32 sub", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("sub32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto result = builder.emitSub(TypeId::INT32, a, b);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();
    REQUIRE(static_cast<int32_t>(run(program, 0, {10, 3})) == 7);
    REQUIRE(static_cast<int32_t>(run(program, 0, {3, 10})) == -7);
}

TEST_CASE("Interpreter: int32 mul", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("mul32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto result = builder.emitMul(TypeId::INT32, a, b);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();
    REQUIRE(static_cast<int32_t>(run(program, 0, {6, 7})) == 42);
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-3), 5})) == -15);
}

TEST_CASE("Interpreter: int32 div and mod", "[interpreter]") {
    // div32
    {
        IRBuilder builder;
        builder.createFunction("div32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto result = builder.emitDiv(TypeId::INT32, a, b);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();
        REQUIRE(static_cast<int32_t>(run(program, 0, {20, 3})) == 6);
        // Division by zero
        REQUIRE(static_cast<int32_t>(run(program, 0, {20, 0})) == 0);
    }

    // mod32
    {
        IRBuilder builder;
        builder.createFunction("mod32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto result = builder.emitMod(TypeId::INT32, a, b);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();
        REQUIRE(static_cast<int32_t>(run(program, 0, {20, 3})) == 2);
        // Mod by zero
        REQUIRE(static_cast<int32_t>(run(program, 0, {20, 0})) == 0);
    }
}

TEST_CASE("Interpreter: int32 neg", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("neg32", TypeId::INT32, {TypeId::INT32});
    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto result = builder.emitNeg(TypeId::INT32, a);
    builder.emitRet(TypeId::INT32, result);
    auto program = builder.finalize();
    REQUIRE(static_cast<int32_t>(run(program, 0, {10})) == -10);
}

// ── Checked sub and checked mul ─────────────────────────────────────────────

TEST_CASE("Interpreter: checked sub (no overflow + overflow)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_sub", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedSub(TypeId::INT32, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT32, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT32, 777);
    builder.emitRet(TypeId::INT32, sentinel);

    auto program = builder.finalize();

    // No overflow
    REQUIRE(static_cast<int32_t>(run(program, 0, {10, 3})) == 7);
    // INT32_MIN - 1 should overflow
    auto minVal = static_cast<uint64_t>(static_cast<uint32_t>(INT32_MIN));
    REQUIRE(static_cast<int32_t>(run(program, 0, {minVal, 1})) == 777);
}

TEST_CASE("Interpreter: checked mul (no overflow + overflow)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_mul", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedMul(TypeId::INT32, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT32, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT32, 888);
    builder.emitRet(TypeId::INT32, sentinel);

    auto program = builder.finalize();

    // No overflow
    REQUIRE(static_cast<int32_t>(run(program, 0, {6, 7})) == 42);
    // INT32_MAX * 2 should overflow
    REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(INT32_MAX), 2})) == 888);
}

TEST_CASE("Interpreter: checked add int64", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_add64", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedAdd(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT64, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT64, 999);
    builder.emitRet(TypeId::INT64, sentinel);

    auto program = builder.finalize();

    // No overflow
    REQUIRE(static_cast<int64_t>(run(program, 0, {100, 200})) == 300);
    // INT64_MAX + 1 should overflow
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(INT64_MAX), 1})) == 999);
}

TEST_CASE("Interpreter: checked sub int64", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_sub64", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedSub(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT64, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT64, 777);
    builder.emitRet(TypeId::INT64, sentinel);

    auto program = builder.finalize();

    REQUIRE(static_cast<int64_t>(run(program, 0, {10, 3})) == 7);
    auto minVal = static_cast<uint64_t>(INT64_MIN);
    REQUIRE(static_cast<int64_t>(run(program, 0, {minVal, 1})) == 777);
}

TEST_CASE("Interpreter: checked mul int64", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("checked_mul64", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto result = builder.emitCheckedMul(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT64, result);

    builder.setInsertPoint(overflowBB);
    auto sentinel = builder.emitConst(TypeId::INT64, 888);
    builder.emitRet(TypeId::INT64, sentinel);

    auto program = builder.finalize();

    REQUIRE(static_cast<int64_t>(run(program, 0, {6, 7})) == 42);
    REQUIRE(static_cast<int64_t>(run(program, 0, {static_cast<uint64_t>(INT64_MAX), 2})) == 888);
}

// ── Int32 shift and rotate ──────────────────────────────────────────────────

TEST_CASE("Interpreter: int32 shift operations", "[interpreter]") {
    // SHL int32
    {
        IRBuilder builder;
        builder.createFunction("shl32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto result = builder.emitShl(TypeId::INT32, a, b);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();
        REQUIRE(static_cast<uint32_t>(run(program, 0, {1, 4})) == 16);
    }

    // ASHR int32
    {
        IRBuilder builder;
        builder.createFunction("ashr32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto result = builder.emitAShr(TypeId::INT32, a, b);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();
        REQUIRE(static_cast<int32_t>(run(program, 0, {static_cast<uint64_t>(-16), 2})) == -4);
    }

    // LSHR int32
    {
        IRBuilder builder;
        builder.createFunction("lshr32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto result = builder.emitLShr(TypeId::INT32, a, b);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();
        REQUIRE(static_cast<uint32_t>(run(program, 0, {0x80000000, 1})) == 0x40000000);
    }
}

TEST_CASE("Interpreter: int32 rotate operations", "[interpreter]") {
    // ROTL int32
    {
        IRBuilder builder;
        builder.createFunction("rotl32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto rotated = builder.emitRotl(TypeId::INT32, a, b);
        auto recovered = builder.emitRotr(TypeId::INT32, rotated, b);
        builder.emitRet(TypeId::INT32, recovered);
        auto program = builder.finalize();
        REQUIRE(static_cast<uint32_t>(run(program, 0, {0xDEADBEEF, 13})) == 0xDEADBEEF);
    }
}

// ── Load/store int8 and ptr types ───────────────────────────────────────────

TEST_CASE("Interpreter: load and store int8", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("store_load8", TypeId::INT8, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitConst(TypeId::INT8, 42);
    builder.emitStore(val, ptr);
    auto loaded = builder.emitLoad(TypeId::INT8, ptr);
    builder.emitRet(TypeId::INT8, loaded);

    auto program = builder.finalize();

    int8_t storage = 0;
    auto ptrVal = reinterpret_cast<uint64_t>(&storage);  // NOLINT
    auto result = static_cast<int8_t>(run(program, 0, {ptrVal}));
    REQUIRE(result == 42);
    REQUIRE(storage == 42);
}

TEST_CASE("Interpreter: load and store ptr type", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("store_load_ptr", TypeId::PTR, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    // Load a ptr from the address
    auto loaded = builder.emitLoad(TypeId::PTR, ptr);
    builder.emitRet(TypeId::PTR, loaded);

    auto program = builder.finalize();

    int64_t target = 99;
    void* targetPtr = &target;
    void* storage = targetPtr;
    auto ptrVal = reinterpret_cast<uint64_t>(&storage);  // NOLINT
    auto result = run(program, 0, {ptrVal});
    REQUIRE(result == reinterpret_cast<uint64_t>(targetPtr));  // NOLINT
}

TEST_CASE("Interpreter: store ptr type", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("store_ptr", TypeId::VOID, {TypeId::PTR, TypeId::PTR});

    auto val = builder.emitFunArg(TypeId::PTR, 0);
    auto dest = builder.emitFunArg(TypeId::PTR, 1);
    builder.emitStore(val, dest);
    builder.emitRetVoid();

    auto program = builder.finalize();

    void* storage = nullptr;
    int64_t target = 42;
    auto targetAddr = reinterpret_cast<uint64_t>(&target);    // NOLINT
    auto storageAddr = reinterpret_cast<uint64_t>(&storage);  // NOLINT
    run(program, 0, {targetAddr, storageAddr});
    REQUIRE(storage == &target);
}

// ── BSWAP 64-bit ────────────────────────────────────────────────────────────

TEST_CASE("Interpreter: byte swap 64-bit", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("bswap64_test", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto swapped = builder.emitBSwap(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, swapped);

    auto program = builder.finalize();

    REQUIRE(run(program, 0, {0x0102030405060708ULL}) == 0x0807060504030201ULL);
}

// ── Truncation edge: 64 bit to 64 bit (no-op) ──────────────────────────────

TEST_CASE("Interpreter: truncation to 64 bits (no-op)", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("trunc_noop", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto truncated = builder.emitTrunc(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, truncated);

    auto program = builder.finalize();
    REQUIRE(run(program, 0, {0xDEADBEEFCAFEBABE}) == 0xDEADBEEFCAFEBABE);
}

// ── Recursive IR function call ──────────────────────────────────────────────

TEST_CASE("Interpreter: call to another IR function", "[interpreter]") {
    IRBuilder builder;

    // Function 0: helper(x) = x * 2
    builder.createFunction("double_it", TypeId::INT64, {TypeId::INT64});
    auto x = builder.emitFunArg(TypeId::INT64, 0);
    auto two = builder.emitConst(TypeId::INT64, 2);
    auto doubled = builder.emitMul(TypeId::INT64, x, two);
    builder.emitRet(TypeId::INT64, doubled);

    // Function 1: caller(a) = double_it(a) + 1
    builder.createFunction("caller", TypeId::INT64, {TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto callResult = builder.emitCall(TypeId::INT64, 0, {a});
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto finalResult = builder.emitAdd(TypeId::INT64, callResult, one);
    builder.emitRet(TypeId::INT64, finalResult);

    auto program = builder.finalize();

    // caller(5) = double_it(5) + 1 = 10 + 1 = 11
    REQUIRE(static_cast<int64_t>(run(program, 1, {5})) == 11);
    REQUIRE(static_cast<int64_t>(run(program, 1, {0})) == 1);
}

// ── NOP instruction ─────────────────────────────────────────────────────────

TEST_CASE("Interpreter: handles NOP instructions after DCE", "[interpreter]") {
    IRBuilder builder;
    builder.createFunction("with_dead_code", TypeId::INT32, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto retBB = builder.createBlock();
    auto deadBB = builder.createBlock();

    builder.emitBr(retBB);

    builder.setInsertPoint(retBB);
    // Add a dead computation that will be NOP'd by DCE
    auto deadVal = builder.emitConst(TypeId::INT32, 999);
    auto one = builder.emitConst(TypeId::INT32, 1);
    auto result = builder.emitAdd(TypeId::INT32, arg, one);
    builder.emitRet(TypeId::INT32, result);

    builder.setInsertPoint(deadBB);
    auto deadConst = builder.emitConst(TypeId::INT32, 0);
    builder.emitRet(TypeId::INT32, deadConst);

    auto program = builder.finalize();
    builder.eliminateDeadCode(program);

    // deadVal should now be NOP, but execution should still work
    REQUIRE(program.instructionAt(deadVal).opcode() == Opcode::NOP);
    REQUIRE(static_cast<int32_t>(run(program, 0, {10})) == 11);
}
