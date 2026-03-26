#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include "backend/flying_start/live_span.h"
#include "backend/flying_start/optimizations.h"
#include "backend/flying_start/register_alloc.h"
#include "backend/flying_start/translator.h"
#include "ir/builder.h"
#include "ir/opcode.h"
#include "ir/program.h"

using namespace umbra::ir;
using umbra::flying_start::FlyingStartCompiler;
using umbra::flying_start::LiveSpanAnalysis;
using umbra::flying_start::RegisterAllocator;

// ── Helper ───────────────────────────────────────────────────────────────────

static uint64_t jitRun(Program& program, FunctionId func, std::vector<uint64_t> args) {
    FlyingStartCompiler jit;
    return jit.compileAndExecute(program, func, std::span<const uint64_t>(args));
}

// ── Optimization 1: Stack space reuse ────────────────────────────────────────

TEST_CASE("JIT Opt: stack space reuse produces correct results", "[jit_opt]") {
    // Many temporary values that go out of scope quickly.
    // Stack space reuse should keep frame size small.
    IRBuilder builder;
    builder.createFunction("many_temps", TypeId::INT64, {TypeId::INT64});

    auto x = builder.emitFunArg(TypeId::INT64, 0);

    // Chain of operations — each intermediate is used only once.
    auto a = builder.emitAdd(TypeId::INT64, x, x);  // 2x
    auto b = builder.emitMul(TypeId::INT64, a, a);  // 4x^2
    auto c = builder.emitAdd(TypeId::INT64, b, x);  // 4x^2 + x
    auto d = builder.emitSub(TypeId::INT64, c, a);  // 4x^2 + x - 2x = 4x^2 - x
    auto e = builder.emitAdd(TypeId::INT64, d, builder.emitConst(TypeId::INT64, 1));
    builder.emitRet(TypeId::INT64, e);

    auto program = builder.finalize();

    // For x=3: 4*9 - 3 + 1 = 34
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {3})) == 34);
    // For x=0: 0 - 0 + 1 = 1
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {0})) == 1);
    // For x=1: 4 - 1 + 1 = 4
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {1})) == 4);
}

// ── Optimization 2: Register allocation ──────────────────────────────────────

TEST_CASE("JIT Opt: register allocation produces correct results", "[jit_opt]") {
    // Loop with multiple live values competing for registers.
    IRBuilder builder;
    builder.createFunction("multi_live", TypeId::INT64,
                           {TypeId::INT64, TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto n = builder.emitFunArg(TypeId::INT64, 2);

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
    // Complex expression using multiple live values.
    auto prod = builder.emitMul(TypeId::INT64, a, phiI);
    auto added = builder.emitAdd(TypeId::INT64, prod, b);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, added);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    // sum(i=0..9) { a*i + b } with a=2, b=3, n=10
    // = sum(3, 5, 7, 9, 11, 13, 15, 17, 19, 21) = 120
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {2, 3, 10})) == 120);
}

TEST_CASE("JIT Opt: register allocator basic operations", "[jit_opt]") {
    RegisterAllocator regAlloc;

    // Should have 9 registers available.
    REQUIRE(regAlloc.availableRegisterCount() == 9);
    REQUIRE(regAlloc.hasAvailableRegister());

    // Allocate all 9.
    std::vector<asmjit::x86::Gp> allocated;
    for (int i = 0; i < 9; ++i) {
        auto reg = regAlloc.allocateRegister();
        REQUIRE(reg.has_value());
        allocated.push_back(*reg);
    }

    // No more available.
    REQUIRE(!regAlloc.hasAvailableRegister());
    REQUIRE(!regAlloc.allocateRegister().has_value());

    // Free one and re-allocate.
    regAlloc.freeRegister(allocated[0]);
    REQUIRE(regAlloc.hasAvailableRegister());
    auto reg = regAlloc.allocateRegister();
    REQUIRE(reg.has_value());
}

TEST_CASE("JIT Opt: stack slot allocation and reuse", "[jit_opt]") {
    RegisterAllocator regAlloc;

    auto slot1 = regAlloc.allocateStackSlot();
    auto slot2 = regAlloc.allocateStackSlot();
    REQUIRE(slot1 != slot2);

    // Free slot1 and re-allocate — should get the same slot.
    regAlloc.freeStackSlot(slot1);
    auto slot3 = regAlloc.allocateStackSlot();
    REQUIRE(slot3 == slot1);  // Reused!

    // Max stack size should reflect the high-water mark.
    REQUIRE(regAlloc.maxStackSize() >= 16);  // At least 2 slots * 8 bytes.
}

// ── Optimization 3: Lazy address calculation ────────────────────────────────

TEST_CASE("JIT Opt: lazy GEP produces correct results", "[jit_opt]") {
    // Test that GEP chains work correctly (address folding).
    IRBuilder builder;
    builder.createFunction("gep_chain", TypeId::INT64, {TypeId::PTR, TypeId::INT64});

    auto basePtr = builder.emitFunArg(TypeId::PTR, 0);
    auto idx = builder.emitFunArg(TypeId::INT64, 1);

    // GEP + load chain.
    auto elemPtr = builder.emitGEP(basePtr, TypeId::INT64, idx);
    auto val = builder.emitLoad(TypeId::INT64, elemPtr);
    builder.emitRet(TypeId::INT64, val);

    auto program = builder.finalize();

    std::vector<int64_t> arr = {100, 200, 300, 400, 500};
    auto arrPtr = reinterpret_cast<uint64_t>(arr.data());  // NOLINT

    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {arrPtr, 0})) == 100);
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {arrPtr, 2})) == 300);
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {arrPtr, 4})) == 500);
}

TEST_CASE("JIT Opt: GEP + store produces correct results", "[jit_opt]") {
    IRBuilder builder;
    builder.createFunction("gep_store", TypeId::VOID, {TypeId::PTR, TypeId::INT64, TypeId::INT64});

    auto basePtr = builder.emitFunArg(TypeId::PTR, 0);
    auto idx = builder.emitFunArg(TypeId::INT64, 1);
    auto val = builder.emitFunArg(TypeId::INT64, 2);

    auto elemPtr = builder.emitGEP(basePtr, TypeId::INT64, idx);
    builder.emitStore(val, elemPtr);
    builder.emitRetVoid();

    auto program = builder.finalize();

    std::vector<int64_t> arr = {0, 0, 0};
    auto arrPtr = reinterpret_cast<uint64_t>(arr.data());  // NOLINT

    jitRun(program, 0, {arrPtr, 1, 42});
    REQUIRE(arr[1] == 42);

    jitRun(program, 0, {arrPtr, 0, 99});
    REQUIRE(arr[0] == 99);
}

// ── Optimization 4: Comparison-branch fusion ────────────────────────────────

TEST_CASE("JIT Opt: fused comparison-branch produces correct results", "[jit_opt]") {
    // This pattern should trigger comparison-branch fusion:
    // cmp + condbr in the same block.
    IRBuilder builder;
    builder.createFunction("fused_cmpbr", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();

    auto cmp = builder.emitCmpLt(TypeId::INT32, a, b);
    builder.emitCondBr(cmp, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitRet(TypeId::INT32, one);

    builder.setInsertPoint(falseBB);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    builder.emitRet(TypeId::INT32, zero);

    auto program = builder.finalize();

    REQUIRE(static_cast<int32_t>(jitRun(program, 0, {3, 5})) == 1);  // 3 < 5
    REQUIRE(static_cast<int32_t>(jitRun(program, 0, {5, 3})) == 0);  // 5 >= 3
    REQUIRE(static_cast<int32_t>(jitRun(program, 0, {5, 5})) == 0);  // 5 >= 5
}

TEST_CASE("JIT Opt: all fused comparison types", "[jit_opt]") {
    auto testFused = [](auto emitCmpFn, int32_t a, int32_t b) -> int32_t {
        IRBuilder builder;
        builder.createFunction("cmp", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto argA = builder.emitFunArg(TypeId::INT32, 0);
        auto argB = builder.emitFunArg(TypeId::INT32, 1);

        auto trueBB = builder.createBlock();
        auto falseBB = builder.createBlock();

        auto cmp = emitCmpFn(builder, argA, argB);
        builder.emitCondBr(cmp, trueBB, falseBB);

        builder.setInsertPoint(trueBB);
        builder.emitRet(TypeId::INT32, builder.emitConst(TypeId::INT32, 1));

        builder.setInsertPoint(falseBB);
        builder.emitRet(TypeId::INT32, builder.emitConst(TypeId::INT32, 0));

        auto program = builder.finalize();
        return static_cast<int32_t>(
            jitRun(program, 0, {static_cast<uint64_t>(a), static_cast<uint64_t>(b)}));
    };

    auto cmpEq = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpEq(TypeId::INT32, l, r); };
    auto cmpNe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpNe(TypeId::INT32, l, r); };
    auto cmpLt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLt(TypeId::INT32, l, r); };
    auto cmpLe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpLe(TypeId::INT32, l, r); };
    auto cmpGt = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGt(TypeId::INT32, l, r); };
    auto cmpGe = [](IRBuilder& b, IROp l, IROp r) { return b.emitCmpGe(TypeId::INT32, l, r); };

    REQUIRE(testFused(cmpEq, 5, 5) == 1);
    REQUIRE(testFused(cmpEq, 5, 3) == 0);
    REQUIRE(testFused(cmpNe, 5, 3) == 1);
    REQUIRE(testFused(cmpNe, 5, 5) == 0);
    REQUIRE(testFused(cmpLt, 3, 5) == 1);
    REQUIRE(testFused(cmpLt, 5, 3) == 0);
    REQUIRE(testFused(cmpLe, 3, 5) == 1);
    REQUIRE(testFused(cmpLe, 5, 5) == 1);
    REQUIRE(testFused(cmpLe, 7, 5) == 0);
    REQUIRE(testFused(cmpGt, 5, 3) == 1);
    REQUIRE(testFused(cmpGt, 3, 5) == 0);
    REQUIRE(testFused(cmpGe, 5, 3) == 1);
    REQUIRE(testFused(cmpGe, 5, 5) == 1);
    REQUIRE(testFused(cmpGe, 3, 5) == 0);
}

// ── Live span analysis ──────────────────────────────────────────────────────

TEST_CASE("JIT Opt: live span analysis computes correct spans", "[jit_opt]") {
    IRBuilder builder;
    builder.createFunction("live_span_test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto result = builder.emitAdd(TypeId::INT64, sum, one);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();

    LiveSpanAnalysis lsa;
    lsa.analyze(program, 0);

    // RPO order should have at least one block.
    REQUIRE(!lsa.rpoOrder().empty());

    // Check that a is used by sum (seq=2).
    const auto* spanA = lsa.getSpan(a);
    REQUIRE(spanA != nullptr);
    REQUIRE(spanA->defPoint < spanA->lastUsePoint);

    // Check that sum is used by result.
    const auto* spanSum = lsa.getSpan(sum);
    REQUIRE(spanSum != nullptr);
    REQUIRE(spanSum->lastUsePoint > spanSum->defPoint);
}

// ── Complex program: NULL propagation pattern ────────────────────────────────

TEST_CASE("JIT Opt: NULL propagation pattern", "[jit_opt]") {
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
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {10, 0, 20, 0})) == 30);
    // a is null: return 0
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {10, 1, 20, 0})) == 0);
    // b is null: return 0
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {10, 0, 20, 1})) == 0);
}

TEST_CASE("JIT Opt: IR constant folding removes trivial logical and cast ops", "[jit_opt]") {
    IRBuilder builder;
    builder.createFunction("fold_consts", TypeId::UINT64, {});

    auto falseConst = builder.emitConst(TypeId::BOOL, 0);
    auto trueConst = builder.emitConst(TypeId::BOOL, 1);
    auto negOne = builder.emitConst(TypeId::INT32, static_cast<uint32_t>(-1));

    auto foldedOr = builder.emitLOr(falseConst, falseConst);
    auto foldedAnd = builder.emitLAnd(trueConst, falseConst);
    auto foldedSExt = builder.emitSExt(TypeId::INT64, negOne);
    auto foldedZExt = builder.emitZExt(TypeId::UINT64, negOne);
    auto foldedBitcast = builder.emitBitcast(TypeId::UINT64, foldedZExt);

    REQUIRE(foldedOr == falseConst);
    REQUIRE(foldedAnd == falseConst);
    REQUIRE(foldedSExt != negOne);
    REQUIRE(foldedZExt != negOne);
    REQUIRE(foldedBitcast == foldedZExt);

    auto result = builder.emitAdd(TypeId::UINT64, foldedZExt, builder.emitConst(TypeId::UINT64, 0));
    builder.emitRet(TypeId::UINT64, result);

    auto program = builder.finalize();
    REQUIRE(jitRun(program, 0, {}) == 0xFFFFFFFFULL);
}

TEST_CASE("JIT Opt: signed constant division avoids idiv", "[jit_opt]") {
    IRBuilder builder;
    builder.createFunction("div_magic", TypeId::INT64, {TypeId::INT64});

    auto value = builder.emitFunArg(TypeId::INT64, 0);
    auto divisor = builder.emitConst(TypeId::INT64, 100);
    auto quotient = builder.emitDiv(TypeId::INT64, value, divisor);
    auto remainder = builder.emitMod(TypeId::INT64, value, divisor);
    auto sum = builder.emitAdd(TypeId::INT64, quotient, remainder);
    builder.emitRet(TypeId::INT64, sum);

    auto program = builder.finalize();

    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {12345})) == 168);
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {static_cast<uint64_t>(-12345LL)})) == -168);

    FlyingStartCompiler jit;
    std::string asmOutput;
    jit.compileWithLog(program, 0, asmOutput);
    REQUIRE(asmOutput.find("idiv") == std::string::npos);
}

TEST_CASE("JIT Opt: deferred GEP folds address arithmetic into memory operands", "[jit_opt]") {
    IRBuilder builder;
    builder.createFunction("gep_folded", TypeId::INT64, {TypeId::PTR, TypeId::INT64});

    auto basePtr = builder.emitFunArg(TypeId::PTR, 0);
    auto idx = builder.emitFunArg(TypeId::INT64, 1);
    auto elemPtr = builder.emitGEP(basePtr, TypeId::INT64, idx);
    auto val = builder.emitLoad(TypeId::INT64, elemPtr);
    builder.emitRet(TypeId::INT64, val);

    auto program = builder.finalize();

    std::vector<int64_t> arr = {11, 22, 33};
    auto arrPtr = reinterpret_cast<uint64_t>(arr.data());  // NOLINT
    REQUIRE(static_cast<int64_t>(jitRun(program, 0, {arrPtr, 2})) == 33);

    FlyingStartCompiler jit;
    std::string asmOutput;
    jit.compileWithLog(program, 0, asmOutput);
    REQUIRE(asmOutput.find("imul") == std::string::npos);
}

// ── Optimization function unit tests ─────────────────────────────────────────

TEST_CASE("JIT Opt: shouldAllocateRegister heuristic", "[jit_opt]") {
    using umbra::flying_start::shouldAllocateRegister;

    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitAdd(TypeId::INT64, a, a);
    builder.emitRet(TypeId::INT64, b);

    auto program = builder.finalize();
    LiveSpanAnalysis lsa;
    lsa.analyze(program, 0);

    RegisterAllocator regAlloc;
    auto entryBlock = lsa.rpoOrder()[0];

    // Value defined in same block should be allocated.
    REQUIRE(shouldAllocateRegister(lsa, a, entryBlock, regAlloc));

    // Exhaust all registers — should not allocate.
    for (int i = 0; i < 9; ++i) {
        regAlloc.allocateRegister();
    }
    REQUIRE(!shouldAllocateRegister(lsa, a, entryBlock, regAlloc));

    // Invalid op should return false.
    REQUIRE(!shouldAllocateRegister(lsa, 999999, entryBlock, regAlloc));
}

TEST_CASE("JIT Opt: canFuseComparisonBranch", "[jit_opt]") {
    using umbra::flying_start::canFuseComparisonBranch;

    // Test fusable case: cmp followed by condbr in same block.
    {
        IRBuilder builder;
        builder.createFunction("fusable", TypeId::INT32, {TypeId::INT32, TypeId::INT32});
        auto a = builder.emitFunArg(TypeId::INT32, 0);
        auto b = builder.emitFunArg(TypeId::INT32, 1);
        auto trueBB = builder.createBlock();
        auto falseBB = builder.createBlock();
        auto cmp = builder.emitCmpEq(TypeId::INT32, a, b);
        builder.emitCondBr(cmp, trueBB, falseBB);
        builder.setInsertPoint(trueBB);
        builder.emitRet(TypeId::INT32, builder.emitConst(TypeId::INT32, 1));
        builder.setInsertPoint(falseBB);
        builder.emitRet(TypeId::INT32, builder.emitConst(TypeId::INT32, 0));

        auto program = builder.finalize();
        LiveSpanAnalysis lsa;
        lsa.analyze(program, 0);

        REQUIRE(canFuseComparisonBranch(program, lsa, cmp));
    }

    // Test non-fusable: non-comparison opcode.
    {
        IRBuilder builder;
        builder.createFunction("nonfusable", TypeId::INT64, {TypeId::INT64});
        auto a = builder.emitFunArg(TypeId::INT64, 0);
        auto b = builder.emitAdd(TypeId::INT64, a, a);
        builder.emitRet(TypeId::INT64, b);

        auto program = builder.finalize();
        LiveSpanAnalysis lsa;
        lsa.analyze(program, 0);

        REQUIRE(!canFuseComparisonBranch(program, lsa, b));
    }
}

TEST_CASE("JIT Opt: canDeferGep", "[jit_opt]") {
    using umbra::flying_start::canDeferGep;

    // GEP used immediately — should be deferrable.
    {
        IRBuilder builder;
        builder.createFunction("gep_defer", TypeId::INT64, {TypeId::PTR, TypeId::INT64});
        auto ptr = builder.emitFunArg(TypeId::PTR, 0);
        auto idx = builder.emitFunArg(TypeId::INT64, 1);
        auto gep = builder.emitGEP(ptr, TypeId::INT64, idx);
        auto val = builder.emitLoad(TypeId::INT64, gep);
        builder.emitRet(TypeId::INT64, val);

        auto program = builder.finalize();
        LiveSpanAnalysis lsa;
        lsa.analyze(program, 0);

        REQUIRE(canDeferGep(program, lsa, gep));
    }

    // Non-GEP opcode — should not be deferrable.
    {
        IRBuilder builder;
        builder.createFunction("nongep", TypeId::INT64, {TypeId::INT64});
        auto a = builder.emitFunArg(TypeId::INT64, 0);
        auto b = builder.emitAdd(TypeId::INT64, a, a);
        builder.emitRet(TypeId::INT64, b);

        auto program = builder.finalize();
        LiveSpanAnalysis lsa;
        lsa.analyze(program, 0);

        REQUIRE(!canDeferGep(program, lsa, b));
    }
}

TEST_CASE("JIT Opt: comparisonConditionCode covers all comparison opcodes", "[jit_opt]") {
    using umbra::flying_start::comparisonConditionCode;
    using umbra::flying_start::inverseConditionCode;

    auto ccEq = comparisonConditionCode(Opcode::CMP_EQ);
    auto ccNe = comparisonConditionCode(Opcode::CMP_NE);
    auto ccLt = comparisonConditionCode(Opcode::CMP_LT);
    auto ccLe = comparisonConditionCode(Opcode::CMP_LE);
    auto ccGt = comparisonConditionCode(Opcode::CMP_GT);
    auto ccGe = comparisonConditionCode(Opcode::CMP_GE);

    // All condition codes should be distinct.
    REQUIRE(ccEq != ccNe);
    REQUIRE(ccLt != ccGe);
    REQUIRE(ccLe != ccGt);

    // Default (non-comparison opcode) returns same as EQ.
    REQUIRE(comparisonConditionCode(Opcode::ADD) == ccEq);

    // Inverse: EQ <-> NE, LT <-> GE, LE <-> GT.
    REQUIRE(inverseConditionCode(ccEq) == ccNe);
    REQUIRE(inverseConditionCode(ccNe) == ccEq);
    REQUIRE(inverseConditionCode(ccLt) == ccGe);
    REQUIRE(inverseConditionCode(ccGe) == ccLt);
    REQUIRE(inverseConditionCode(ccLe) == ccGt);
    REQUIRE(inverseConditionCode(ccGt) == ccLe);

    // Unknown cc returns unchanged.
    REQUIRE(inverseConditionCode(255) == 255);
}

TEST_CASE("JIT Opt: register allocator value tracking", "[jit_opt]") {
    using umbra::flying_start::LocationKind;
    using umbra::flying_start::ValueLocation;

    RegisterAllocator regAlloc;

    // Set a register location.
    auto reg = regAlloc.allocateRegister();
    REQUIRE(reg.has_value());
    ValueLocation regLoc{LocationKind::REGISTER, *reg, 0};
    regAlloc.setLocation(100, regLoc);

    const auto* loc = regAlloc.getLocation(100);
    REQUIRE(loc != nullptr);
    REQUIRE(loc->kind == LocationKind::REGISTER);

    // Find value by register.
    REQUIRE(regAlloc.findValueInReg(*reg) == 100);

    // Remove location should free the register.
    int before = regAlloc.availableRegisterCount();
    regAlloc.removeLocation(100);
    REQUIRE(regAlloc.availableRegisterCount() == before + 1);
    REQUIRE(regAlloc.getLocation(100) == nullptr);
    REQUIRE(regAlloc.findValueInReg(*reg) == INVALID_OP);

    // Stack location tracking.
    auto slot = regAlloc.allocateStackSlot();
    ValueLocation stackLoc{LocationKind::STACK, asmjit::x86::Gp(), slot};
    regAlloc.setLocation(200, stackLoc);
    regAlloc.removeLocation(200);
    REQUIRE(regAlloc.getLocation(200) == nullptr);
}

TEST_CASE("JIT Opt: scratch registers and callee-saved tracking", "[jit_opt]") {
    RegisterAllocator regAlloc;

    // Scratch registers are accessible.
    REQUIRE(RegisterAllocator::scratchReg(0).id() == asmjit::x86::rax.id());
    REQUIRE(RegisterAllocator::scratchReg(1).id() == asmjit::x86::rcx.id());
    REQUIRE(RegisterAllocator::scratchReg(2).id() == asmjit::x86::rdx.id());
    REQUIRE(RegisterAllocator::scratchReg(3).id() == asmjit::x86::rsi.id());

    // Allocate all 9 regs — usedCalleeSaved should have 5 (rbx, r12-r15).
    for (int i = 0; i < 9; ++i) {
        regAlloc.allocateRegister();
    }
    REQUIRE(regAlloc.usedCalleeSaved().size() == 5);
}

TEST_CASE("JIT Opt: register allocator spillToStack", "[jit_opt]") {
    RegisterAllocator regAlloc;

    auto reg = regAlloc.allocateRegister();
    REQUIRE(reg.has_value());
    int countBefore = regAlloc.availableRegisterCount();

    int32_t stackOff = regAlloc.spillToStack(*reg);
    // After spill, register should be freed.
    REQUIRE(regAlloc.availableRegisterCount() == countBefore + 1);
    REQUIRE(stackOff < 0);  // Negative offset from rbp.
}

TEST_CASE("JIT Opt: liveCallerSaved returns only live caller-saved regs", "[jit_opt]") {
    RegisterAllocator regAlloc;

    // Allocate all 9 regs — first 4 should be caller-saved (r8-r11).
    std::vector<asmjit::x86::Gp> regs;
    for (int i = 0; i < 9; ++i) {
        auto reg = regAlloc.allocateRegister();
        REQUIRE(reg.has_value());
        regs.push_back(*reg);
    }

    auto callerSaved = regAlloc.liveCallerSaved();
    REQUIRE(callerSaved.size() == 4);  // r8, r9, r10, r11.

    // Free a caller-saved reg.
    regAlloc.freeRegister(regs[0]);
    callerSaved = regAlloc.liveCallerSaved();
    REQUIRE(callerSaved.size() == 3);

    // usedCalleeSaved should track callee-saved regs.
    REQUIRE(regAlloc.usedCalleeSaved().size() == 5);
}

TEST_CASE("JIT Opt: shouldAllocateRegister cross-block with plenty of regs", "[jit_opt]") {
    using umbra::flying_start::shouldAllocateRegister;

    // Build a two-block program where a value defined in block 0 is used in block 1.
    IRBuilder builder;
    builder.createFunction("cross_block", TypeId::INT64, {TypeId::INT64});
    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto secondBB = builder.createBlock();
    builder.emitBr(secondBB);

    builder.setInsertPoint(secondBB);
    auto result = builder.emitAdd(TypeId::INT64, a, a);
    builder.emitRet(TypeId::INT64, result);

    auto program = builder.finalize();
    LiveSpanAnalysis lsa;
    lsa.analyze(program, 0);

    RegisterAllocator regAlloc;
    auto secondBlock = lsa.rpoOrder()[1];

    // Cross-block value with > 3 free regs should still allocate.
    REQUIRE(regAlloc.availableRegisterCount() > 3);
    REQUIRE(shouldAllocateRegister(lsa, a, secondBlock, regAlloc));

    // Exhaust down to 3 regs — should no longer allocate cross-block.
    while (regAlloc.availableRegisterCount() > 3) {
        regAlloc.allocateRegister();
    }
    REQUIRE(!shouldAllocateRegister(lsa, a, secondBlock, regAlloc));
}
