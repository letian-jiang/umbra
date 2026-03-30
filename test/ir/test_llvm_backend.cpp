#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <span>
#include <vector>

#include "backend/flying_start/translator.h"
#include "backend/interpreter/interpreter.h"
#include "backend/llvm/llvm_compiler.h"
#include "ir/builder.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::ir;
using umbra::flying_start::FlyingStartCompiler;
using umbra::interpreter::Interpreter;
using umbra::llvm_backend::LLVMCompiler;

// ── Cross-validation helper ─────────────────────────────────────────────────

static void crossValidate3(Program& program, FunctionId func, std::vector<uint64_t> args,
                           const char* label) {
    INFO("Cross-validating (3 backends): " << label);

    Interpreter interp;
    uint64_t interpResult = interp.execute(program, func, std::span<const uint64_t>(args));

    FlyingStartCompiler fs;
    uint64_t fsResult = fs.compileAndExecute(program, func, std::span<const uint64_t>(args));

    LLVMCompiler llvm;
    uint64_t llvmResult = llvm.compileAndExecute(program, func, std::span<const uint64_t>(args));

    REQUIRE(interpResult == fsResult);
    REQUIRE(interpResult == llvmResult);
}

// ── Simple arithmetic ───────────────────────────────────────────────────────

TEST_CASE("LLVM: simple add (IR capture)", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("add_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, sum);

    auto program = builder.finalize();

    LLVMCompiler llvm;
    std::string irText;
    void* fn = llvm.compileWithLog(program, 0, irText);
    INFO("LLVM IR:\n" << irText);
    REQUIRE(fn != nullptr);

    uint64_t result = LLVMCompiler::execute(fn, std::span<const uint64_t>(std::vector<uint64_t>{5, 3}));
    REQUIRE(result == 8);
}

TEST_CASE("LLVM: simple add", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("add_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, sum);

    auto program = builder.finalize();

    crossValidate3(program, 0, {5, 3}, "5 + 3");
    crossValidate3(program, 0, {0, 0}, "0 + 0");
    crossValidate3(program, 0, {static_cast<uint64_t>(-10), 3}, "-10 + 3");
}

TEST_CASE("LLVM: arithmetic (a+b)*(a-b)", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("diff_product", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto diff = builder.emitSub(TypeId::INT64, a, b);
    auto prod = builder.emitMul(TypeId::INT64, sum, diff);
    builder.emitRet(TypeId::INT64, prod);

    auto program = builder.finalize();

    crossValidate3(program, 0, {5, 3}, "(5+3)*(5-3)");
    crossValidate3(program, 0, {10, 7}, "(10+7)*(10-7)");
}

TEST_CASE("LLVM: int32 arithmetic", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("add32", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto sum = builder.emitAdd(TypeId::INT32, a, b);
    builder.emitRet(TypeId::INT32, sum);

    auto program = builder.finalize();

    crossValidate3(program, 0, {100, 200}, "100 + 200 (int32)");
}

TEST_CASE("LLVM: division and modulo", "[llvm]") {
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

    crossValidate3(program, 0, {17, 3}, "17 divmod 3");
    crossValidate3(program, 0, {100, 7}, "100 divmod 7");
}

TEST_CASE("LLVM: conditional branch", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("max_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    auto cmp = builder.emitCmpGt(TypeId::INT64, a, b);
    builder.emitCondBr(cmp, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(falseBB);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT64, {{trueBB, a}, {falseBB, b}});
    builder.emitRet(TypeId::INT64, phi);

    auto program = builder.finalize();

    crossValidate3(program, 0, {10, 5}, "max(10, 5)");
    crossValidate3(program, 0, {3, 7}, "max(3, 7)");
    crossValidate3(program, 0, {5, 5}, "max(5, 5)");
}

TEST_CASE("LLVM: loop with PHI (sum 0..N-1)", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("sum_n", TypeId::INT64, {TypeId::INT64});

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

    // sum(0..9) = 45, sum(0..99) = 4950, sum(empty) = 0
    crossValidate3(program, 0, {10}, "sum(0..9) = 45");
    crossValidate3(program, 0, {100}, "sum(0..99) = 4950");
    crossValidate3(program, 0, {0}, "sum(empty) = 0");
}

TEST_CASE("LLVM: host function call", "[llvm]") {
    // Function 0 calls host function at ID 1.
    IRBuilder builder;
    builder.createFunction("test_call", TypeId::INT64, {TypeId::INT64, TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    // emitCall with function ID 1 (will be host function).
    auto result = builder.emitCall(TypeId::INT64, 1, {a, b});
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    // Register host function at ID 1.
    program.registerHostFunction(
        "host_add",
        [](const uint64_t* args, uint8_t count) -> uint64_t {
            (void)count;
            return args[0] + args[1];
        },
        TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    crossValidate3(program, 0, {10, 20}, "host_add(10, 20)");
    crossValidate3(program, 0, {0, 0}, "host_add(0, 0)");
}

TEST_CASE("LLVM: host function call with signed narrow args", "[llvm]") {
    // Regression: LLVM must sign-extend (not zero-extend) narrow integer args
    // to match the interpreter/Flying Start ABI where slots are sign-extended.
    IRBuilder builder;
    builder.createFunction("test_narrow", TypeId::INT64, {TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    // Truncate to INT8 to get a narrow signed value.
    auto narrow = builder.emitTrunc(TypeId::INT8, a);
    // Pass the INT8 to a host function — must arrive sign-extended.
    auto result = builder.emitCall(TypeId::INT64, 1, {narrow});
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    // Host function that returns its argument unchanged — lets us observe the widened value.
    program.registerHostFunction(
        "host_identity",
        [](const uint64_t* args, uint8_t /*count*/) -> uint64_t { return args[0]; },
        TypeId::INT64, {TypeId::INT8});

    // INT8(-1) = 0xFF. Sign-extended to i64: 0xFFFFFFFFFFFFFFFF = uint64_t(-1).
    // Zero-extended would give 0xFF = 255 — wrong.
    crossValidate3(program, 0, {static_cast<uint64_t>(-1)}, "INT8(-1) sign-extend");
    crossValidate3(program, 0, {127}, "INT8(127) positive value");
    crossValidate3(program, 0, {static_cast<uint64_t>(-128)}, "INT8(-128) sign-extend");
}

TEST_CASE("LLVM: memory load/store", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("mem_fn", TypeId::INT64, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitLoad(TypeId::INT64, ptr);
    auto doubled = builder.emitMul(TypeId::INT64, val, builder.emitConst(TypeId::INT64, 2));
    builder.emitStore(doubled, ptr);
    builder.emitRet(TypeId::INT64, doubled);

    auto program = builder.finalize();

    int64_t data = 21;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto ptrArg = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&data));

    LLVMCompiler llvm;
    uint64_t result = llvm.compileAndExecute(program, 0, std::span<const uint64_t>({ptrArg}));
    REQUIRE(result == 42);
    REQUIRE(data == 42);
}

TEST_CASE("LLVM: comparison operators", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("cmp_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto eq = builder.emitCmpEq(TypeId::INT64, a, b);
    auto lt = builder.emitCmpLt(TypeId::INT64, a, b);
    auto ge = builder.emitCmpGe(TypeId::INT64, a, b);

    // Pack: eq | (lt << 1) | (ge << 2)
    auto eq64 = builder.emitZExt(TypeId::INT64, eq);
    auto lt64 = builder.emitZExt(TypeId::INT64, lt);
    auto ge64 = builder.emitZExt(TypeId::INT64, ge);
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto two = builder.emitConst(TypeId::INT64, 2);
    auto lt_shifted = builder.emitShl(TypeId::INT64, lt64, one);
    auto ge_shifted = builder.emitShl(TypeId::INT64, ge64, two);
    auto result = builder.emitOr(TypeId::INT64, eq64, lt_shifted);
    auto result2 = builder.emitOr(TypeId::INT64, result, ge_shifted);
    builder.emitRet(TypeId::INT64, result2);

    auto program = builder.finalize();

    crossValidate3(program, 0, {5, 5}, "cmp(5, 5)");   // eq=1, lt=0, ge=1 → 0b101 = 5
    crossValidate3(program, 0, {3, 7}, "cmp(3, 7)");   // eq=0, lt=1, ge=0 → 0b010 = 2
    crossValidate3(program, 0, {7, 3}, "cmp(7, 3)");   // eq=0, lt=0, ge=1 → 0b100 = 4
}

TEST_CASE("LLVM: cast operations", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("cast_fn", TypeId::INT64, {TypeId::INT64});

    auto val = builder.emitFunArg(TypeId::INT64, 0);
    auto truncated = builder.emitTrunc(TypeId::INT32, val);
    auto extended = builder.emitSExt(TypeId::INT64, truncated);
    builder.emitRet(TypeId::INT64, extended);

    auto program = builder.finalize();

    crossValidate3(program, 0, {0x00000000FFFFFFFF}, "trunc+sext (0xFFFFFFFF -> -1)");
    crossValidate3(program, 0, {42}, "trunc+sext (42)");
}

TEST_CASE("LLVM: select operation", "[llvm]") {
    IRBuilder builder;
    builder.createFunction("select_fn", TypeId::INT64, {TypeId::INT64, TypeId::INT64, TypeId::INT64});

    auto cond = builder.emitFunArg(TypeId::INT64, 0);
    auto tv = builder.emitFunArg(TypeId::INT64, 1);
    auto fv = builder.emitFunArg(TypeId::INT64, 2);

    auto condBool = builder.emitTrunc(TypeId::BOOL, cond);
    auto result = builder.emitSelect(TypeId::INT64, condBool, tv, fv);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    crossValidate3(program, 0, {1, 10, 20}, "select(true, 10, 20)");
    crossValidate3(program, 0, {0, 10, 20}, "select(false, 10, 20)");
}
