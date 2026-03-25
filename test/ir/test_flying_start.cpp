#include <catch2/catch_test_macros.hpp>
#include <climits>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "backend/flying_start/translator.h"
#include "backend/interpreter/interpreter.h"
#include "ir/builder.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::ir;
using umbra::flying_start::FlyingStartCompiler;
using umbra::interpreter::Interpreter;

// ── Cross-validation helper ──────────────────────────────────────────────────

/// Run the same program through both interpreter and JIT, compare results.
static void crossValidate(Program& program, FunctionId func, std::vector<uint64_t> args,
                          const char* label) {
    INFO("Cross-validating: " << label);

    Interpreter interp;
    uint64_t interpResult = interp.execute(program, func, std::span<const uint64_t>(args));

    FlyingStartCompiler jit;
    uint64_t jitResult = jit.compileAndExecute(program, func, std::span<const uint64_t>(args));

    REQUIRE(interpResult == jitResult);
}

// ── Simple arithmetic ────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: simple add", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("add_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, sum);

    auto program = builder.finalize();

    crossValidate(program, 0, {5, 3}, "5 + 3");
    crossValidate(program, 0, {0, 0}, "0 + 0");
    crossValidate(program, 0, {static_cast<uint64_t>(-10), 3}, "-10 + 3");
}

TEST_CASE("FlyingStart: arithmetic (a+b)*(a-b)", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("diff_product", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto diff = builder.emitSub(TypeId::INT64, a, b);
    auto prod = builder.emitMul(TypeId::INT64, sum, diff);
    builder.emitRet(TypeId::INT64, prod);

    auto program = builder.finalize();

    crossValidate(program, 0, {5, 3}, "(5+3)*(5-3)");
    crossValidate(program, 0, {10, 7}, "(10+7)*(10-7)");
    crossValidate(program, 0, {0, 0}, "(0+0)*(0-0)");
}

TEST_CASE("FlyingStart: int32 arithmetic", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("add32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto sum = builder.emitAdd(TypeId::INT32, a, b);
    builder.emitRet(TypeId::INT32, sum);

    auto program = builder.finalize();

    crossValidate(program, 0, {100, 200}, "100 + 200 (int32)");
    crossValidate(program, 0, {static_cast<uint64_t>(-10), 3}, "-10 + 3 (int32)");
}

TEST_CASE("FlyingStart: division and modulo", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("divmod", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto quotient = builder.emitDiv(TypeId::INT64, a, b);
    auto remainder = builder.emitMod(TypeId::INT64, a, b);
    auto prod = builder.emitMul(TypeId::INT64, quotient, b);
    auto result = builder.emitAdd(TypeId::INT64, prod, remainder);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    crossValidate(program, 0, {17, 3}, "17 divmod 3");
    crossValidate(program, 0, {100, 7}, "100 divmod 7");
}

TEST_CASE("FlyingStart: unsigned division", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("udiv_test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto a = builder.emitFunArg(TypeId::UINT64, 0);
    auto b = builder.emitFunArg(TypeId::UINT64, 1);
    auto result = builder.emitUDiv(TypeId::UINT64, a, b);
    builder.emitRet(TypeId::UINT64, result);

    auto program = builder.finalize();

    crossValidate(program, 0, {100, 3}, "100 udiv 3");
    crossValidate(program, 0, {100, 0}, "100 udiv 0");
}

TEST_CASE("FlyingStart: negation", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("neg_test", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto negA = builder.emitNeg(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, negA);

    auto program = builder.finalize();

    crossValidate(program, 0, {42}, "neg 42");
    crossValidate(program, 0, {static_cast<uint64_t>(-7)}, "neg -7");
}

// ── Comparison and branching ─────────────────────────────────────────────────

TEST_CASE("FlyingStart: conditional branch (abs value)", "[flying_start]") {
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

    crossValidate(program, 0, {5}, "abs(5)");
    crossValidate(program, 0, {static_cast<uint64_t>(-5)}, "abs(-5)");
    crossValidate(program, 0, {0}, "abs(0)");
}

TEST_CASE("FlyingStart: all comparison operators", "[flying_start]") {
    auto testCmp = [](auto emitCmpFn, int32_t a, int32_t b, const char* label) {
        IRBuilder builder;
        builder.createFunction("cmp", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});
        auto argA = builder.emitFunArg(TypeId::INT32, 0);
        auto argB = builder.emitFunArg(TypeId::INT32, 1);
        auto result = emitCmpFn(builder, argA, argB);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        crossValidate(program, 0, {static_cast<uint64_t>(a), static_cast<uint64_t>(b)}, label);
    };

    auto cmpEq = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpEq(TypeId::INT32, l, r); };
    auto cmpNe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpNe(TypeId::INT32, l, r); };
    auto cmpLt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLt(TypeId::INT32, l, r); };
    auto cmpLe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLe(TypeId::INT32, l, r); };
    auto cmpGt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGt(TypeId::INT32, l, r); };
    auto cmpGe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGe(TypeId::INT32, l, r); };

    testCmp(cmpEq, 5, 5, "5 == 5");
    testCmp(cmpEq, 5, 3, "5 == 3");
    testCmp(cmpNe, 5, 3, "5 != 3");
    testCmp(cmpNe, 5, 5, "5 != 5");
    testCmp(cmpLt, 3, 5, "3 < 5");
    testCmp(cmpLt, 5, 3, "5 < 3");
    testCmp(cmpLe, 3, 5, "3 <= 5");
    testCmp(cmpLe, 5, 5, "5 <= 5");
    testCmp(cmpGt, 5, 3, "5 > 3");
    testCmp(cmpGt, 3, 5, "3 > 5");
    testCmp(cmpGe, 5, 3, "5 >= 3");
    testCmp(cmpGe, 5, 5, "5 >= 5");
}

// ── Memory load/store ────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: memory load and store", "[flying_start]") {
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

    // Use separate memory for each backend since both modify the value.
    int64_t valueInterp = 42;
    Interpreter interp;
    auto interpResult = interp.execute(
        program, 0, std::vector<uint64_t>{reinterpret_cast<uint64_t>(&valueInterp)});  // NOLINT

    int64_t valueJit = 42;
    FlyingStartCompiler jit;
    auto jitResult = jit.compileAndExecute(
        program, 0, std::vector<uint64_t>{reinterpret_cast<uint64_t>(&valueJit)});  // NOLINT

    REQUIRE(interpResult == jitResult);
}

TEST_CASE("FlyingStart: array sum with GEP", "[flying_start]") {
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
    crossValidate(program, 0, {arrPtr, 5}, "array sum [10,20,30,40,50]");
    crossValidate(program, 0, {arrPtr, 0}, "array sum empty");
    crossValidate(program, 0, {arrPtr, 1}, "array sum [10]");
}

// ── Loops with PHI nodes ─────────────────────────────────────────────────────

TEST_CASE("FlyingStart: loop sum 0..n-1", "[flying_start]") {
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
    auto cmp = builder.emitCmpLt(TypeId::INT64, phiI, n);
    builder.emitCondBr(cmp, bodyBB, exitBB);

    builder.setInsertPoint(bodyBB);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, phiI);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    crossValidate(program, 0, {10}, "sum(0..9)");
    crossValidate(program, 0, {100}, "sum(0..99)");
    crossValidate(program, 0, {0}, "sum(0)");
    crossValidate(program, 0, {1}, "sum(0..0)");
}

TEST_CASE("FlyingStart: factorial", "[flying_start]") {
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
    auto cmp = builder.emitCmpLe(TypeId::INT64, phiI, n);
    builder.emitCondBr(cmp, bodyBB, exitBB);

    builder.setInsertPoint(bodyBB);
    auto newResult = builder.emitMul(TypeId::INT64, phiResult, phiI);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiResult, 1, newResult);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiResult);

    auto program = builder.finalize();

    crossValidate(program, 0, {1}, "factorial(1)");
    crossValidate(program, 0, {5}, "factorial(5)");
    crossValidate(program, 0, {10}, "factorial(10)");
}

// ── Host function calls ──────────────────────────────────────────────────────

TEST_CASE("FlyingStart: host function call", "[flying_start]") {
    IRBuilder builder;

    auto hostFn = [](const uint64_t* args, uint8_t count) -> uint64_t {
        if (count >= 2) {
            return static_cast<uint64_t>(static_cast<int64_t>(args[0]) *
                                         static_cast<int64_t>(args[1]));
        }
        return 0;
    };

    builder.createFunction("caller", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto callResult = builder.emitCall(TypeId::INT64, 1, {a, b});
    builder.emitRet(TypeId::INT64, callResult);

    auto program = builder.finalize();
    program.registerHostFunction("multiply", HostFunction(hostFn), TypeId::INT64,
                                 {TypeId::INT64, TypeId::INT64});

    crossValidate(program, 0, {6, 7}, "host multiply(6, 7)");
    crossValidate(program, 0, {static_cast<uint64_t>(-3), 4}, "host multiply(-3, 4)");
}

// ── CRC32 ────────────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: CRC32 instruction", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("hash", TypeId::UINT64, {TypeId::UINT64});

    auto data = builder.emitFunArg(TypeId::UINT64, 0);
    auto seed = builder.emitConst(TypeId::UINT64, 0);
    auto hash = builder.emitCRC32(data, seed);
    builder.emitRet(TypeId::UINT64, hash);

    auto program = builder.finalize();

    // CRC32 hardware instruction may differ from software implementation.
    // So we just verify the JIT version produces consistent results.
    FlyingStartCompiler jit;
    auto result0 = jit.compileAndExecute(program, 0, std::vector<uint64_t>{0});
    FlyingStartCompiler jit2;
    auto result1 = jit2.compileAndExecute(program, 0, std::vector<uint64_t>{1});

    REQUIRE(result0 != result1);

    // Same input → same output.
    FlyingStartCompiler jit3;
    REQUIRE(jit3.compileAndExecute(program, 0, std::vector<uint64_t>{42}) == [&] {
        FlyingStartCompiler j;
        return j.compileAndExecute(program, 0, std::vector<uint64_t>{42});
    }());
}

// ── Bitwise operations ──────────────────────────────────────────────────────

TEST_CASE("FlyingStart: bitwise operations", "[flying_start]") {
    auto testBitwise = [](auto emitFn, uint64_t a, uint64_t b, const char* label) {
        IRBuilder builder;
        builder.createFunction("bitop", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
        auto argA = builder.emitFunArg(TypeId::INT64, 0);
        auto argB = builder.emitFunArg(TypeId::INT64, 1);
        auto result = emitFn(builder, argA, argB);
        builder.emitRet(TypeId::INT64, result);
        auto program = builder.finalize();
        crossValidate(program, 0, {a, b}, label);
    };

    auto andFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitAnd(TypeId::INT64, l, r); };
    auto orFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitOr(TypeId::INT64, l, r); };
    auto xorFn = [](IRBuilder& b, IROp l, IROp r) { return b.emitXor(TypeId::INT64, l, r); };

    testBitwise(andFn, 0xFF, 0x0F, "0xFF & 0x0F");
    testBitwise(orFn, 0xF0, 0x0F, "0xF0 | 0x0F");
    testBitwise(xorFn, 0xFF, 0x0F, "0xFF ^ 0x0F");
}

TEST_CASE("FlyingStart: shift operations", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("shifts", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto four = builder.emitConst(TypeId::INT64, 4);
    auto shl = builder.emitShl(TypeId::INT64, a, four);
    auto lshr = builder.emitLShr(TypeId::INT64, shl, four);
    builder.emitRet(TypeId::INT64, lshr);

    auto program = builder.finalize();
    crossValidate(program, 0, {0xFF}, "shl+lshr");
}

TEST_CASE("FlyingStart: arithmetic shift right", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("ashr_test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto result = builder.emitAShr(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    crossValidate(program, 0, {static_cast<uint64_t>(-16), 2}, "-16 >> 2");
    crossValidate(program, 0, {16, 2}, "16 >> 2");
}

// ── Logical operations ──────────────────────────────────────────────────────

TEST_CASE("FlyingStart: logical operations", "[flying_start]") {
    // LNOT
    {
        IRBuilder builder;
        builder.createFunction("lnot_test", TypeId::BOOL, {TypeId::BOOL});
        auto a = builder.emitFunArg(TypeId::BOOL, 0);
        auto result = builder.emitLNot(a);
        builder.emitRet(TypeId::BOOL, result);
        auto program = builder.finalize();
        crossValidate(program, 0, {1}, "lnot(1)");
        crossValidate(program, 0, {0}, "lnot(0)");
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
        crossValidate(program, 0, {1, 1}, "1 land 1");
        crossValidate(program, 0, {1, 0}, "1 land 0");
        crossValidate(program, 0, {0, 0}, "0 land 0");
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
        crossValidate(program, 0, {1, 1}, "1 lor 1");
        crossValidate(program, 0, {0, 1}, "0 lor 1");
        crossValidate(program, 0, {0, 0}, "0 lor 0");
    }
}

// ── Select ──────────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: select (branchless min)", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("branchless_min", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto cond = builder.emitCmpLt(TypeId::INT32, a, b);
    auto result = builder.emitSelect(TypeId::INT32, cond, a, b);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();

    crossValidate(program, 0, {3, 7}, "min(3,7)");
    crossValidate(program, 0, {7, 3}, "min(7,3)");
    crossValidate(program, 0, {5, 5}, "min(5,5)");
}

// ── Cast operations ─────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: sign extension", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("sext_test", TypeId::INT64, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto extended = builder.emitSExt(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, extended);

    auto program = builder.finalize();

    crossValidate(program, 0, {42}, "sext(42)");
    crossValidate(program, 0, {static_cast<uint64_t>(-1) & 0xFFFFFFFF}, "sext(-1)");
}

TEST_CASE("FlyingStart: zero extension", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("zext_test", TypeId::INT64, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto extended = builder.emitZExt(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, extended);

    auto program = builder.finalize();

    crossValidate(program, 0, {42}, "zext(42)");
}

TEST_CASE("FlyingStart: truncation", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("trunc_test", TypeId::INT32, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto truncated = builder.emitTrunc(TypeId::INT32, a);
    builder.emitRet(TypeId::INT32, truncated);

    auto program = builder.finalize();

    crossValidate(program, 0, {42}, "trunc(42)");
}

TEST_CASE("FlyingStart: bitcast", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("cast_test", TypeId::UINT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto casted = builder.emitBitcast(TypeId::UINT64, a);
    builder.emitRet(TypeId::UINT64, casted);

    auto program = builder.finalize();
    crossValidate(program, 0, {0xDEADBEEF}, "bitcast");
}

// ── Constants ───────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: constant values", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("const_test", TypeId::INT64, {});

    auto val = builder.emitConst(TypeId::INT64, 0xDEADBEEFCAFEBABE);
    builder.emitRet(TypeId::INT64, val);

    auto program = builder.finalize();
    crossValidate(program, 0, {}, "const 0xDEADBEEFCAFEBABE");
}

// ── RetVoid ─────────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: ret void", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("void_fn", TypeId::VOID, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitConst(TypeId::INT64, 42);
    builder.emitStore(val, ptr);
    builder.emitRetVoid();

    auto program = builder.finalize();

    // Run through JIT.
    int64_t result = 0;
    auto ptrVal = reinterpret_cast<uint64_t>(&result);  // NOLINT
    FlyingStartCompiler jit;
    jit.compileAndExecute(program, 0, std::vector<uint64_t>{ptrVal});
    REQUIRE(result == 42);
}

// ── BSWAP ───────────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: byte swap", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("bswap_test", TypeId::INT32, {TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto swapped = builder.emitBSwap(TypeId::INT32, a);
    builder.emitRet(TypeId::INT32, swapped);

    auto program = builder.finalize();
    crossValidate(program, 0, {0x01020304}, "bswap32");
}

TEST_CASE("FlyingStart: byte swap 64-bit", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("bswap64_test", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto swapped = builder.emitBSwap(TypeId::INT64, a);
    builder.emitRet(TypeId::INT64, swapped);

    auto program = builder.finalize();
    crossValidate(program, 0, {0x0102030405060708ULL}, "bswap64");
}

// ── IS_NULL ─────────────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: IS_NULL instruction", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("check_null", TypeId::BOOL, {TypeId::BOOL});

    auto nullFlag = builder.emitFunArg(TypeId::BOOL, 0);
    auto isNull = builder.emitIsNull(nullFlag);
    builder.emitRet(TypeId::BOOL, isNull);

    auto program = builder.finalize();
    crossValidate(program, 0, {1}, "isnull(1)");
    crossValidate(program, 0, {0}, "isnull(0)");
}

// ── Checked arithmetic ──────────────────────────────────────────────────────

TEST_CASE("FlyingStart: checked add (no overflow)", "[flying_start]") {
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
    auto sentinel = builder.emitConst(TypeId::INT32, 999);
    builder.emitRet(TypeId::INT32, sentinel);

    auto program = builder.finalize();
    crossValidate(program, 0, {100, 200}, "checked_add(100, 200)");
}

TEST_CASE("FlyingStart: checked add (overflow)", "[flying_start]") {
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
    crossValidate(program, 0, {static_cast<uint64_t>(INT32_MAX), 1}, "checked_add(INT32_MAX, 1)");
}

// ── Store/load int32 ────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: store and load int32", "[flying_start]") {
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
    crossValidate(program, 0, {ptrVal}, "store/load int32");
}

// ── PHI with 3 incoming edges ───────────────────────────────────────────────

TEST_CASE("FlyingStart: phi3 (sign function)", "[flying_start]") {
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

    crossValidate(program, 0, {static_cast<uint64_t>(-42)}, "signum(-42)");
    crossValidate(program, 0, {42}, "signum(42)");
    crossValidate(program, 0, {0}, "signum(0)");
}

// ── Division by zero ────────────────────────────────────────────────────────

TEST_CASE("FlyingStart: division by zero returns 0", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("div_zero", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    auto result = builder.emitDiv(TypeId::INT64, a, zero);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    crossValidate(program, 0, {42}, "42 / 0");
}

TEST_CASE("FlyingStart: mod by zero returns 0", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("mod_zero", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    auto result = builder.emitMod(TypeId::INT64, a, zero);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    crossValidate(program, 0, {42}, "42 % 0");
}

// ── Rotate operations ───────────────────────────────────────────────────────

TEST_CASE("FlyingStart: rotate operations", "[flying_start]") {
    IRBuilder builder;
    builder.createFunction("rotate_test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto val = builder.emitFunArg(TypeId::UINT64, 0);
    auto amount = builder.emitFunArg(TypeId::UINT64, 1);
    auto rotated = builder.emitRotl(TypeId::UINT64, val, amount);
    auto recovered = builder.emitRotr(TypeId::UINT64, rotated, amount);
    builder.emitRet(TypeId::UINT64, recovered);

    auto program = builder.finalize();
    crossValidate(program, 0, {0xDEADBEEF12345678ULL, 17}, "rotl+rotr");
}
