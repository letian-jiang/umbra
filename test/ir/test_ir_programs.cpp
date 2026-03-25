#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ir/builder.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/printer.h"
#include "ir/program.h"

using namespace umbra::ir;

// ── Hand-written IR programs ─────────────────────────────────────────────────
// These test programs validate the IR builder for realistic code patterns:
// arithmetic, control flow, memory, PHI nodes.

TEST_CASE("IR program: simple arithmetic", "[ir-programs]") {
    // Compute: f(a, b) = (a + b) * (a - b)
    IRBuilder builder;
    builder.createFunction("diff_product", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    auto diff = builder.emitSub(TypeId::INT64, a, b);
    auto prod = builder.emitMul(TypeId::INT64, sum, diff);
    builder.emitRet(TypeId::INT64, prod);

    auto program = builder.finalize();

    // Verify structure.
    REQUIRE(program.numFunctions() == 1);
    auto& meta = program.functionMeta(0);
    REQUIRE(meta.name == "diff_product");
    REQUIRE(meta.returnType == TypeId::INT64);
    REQUIRE(meta.numParams == 2);

    // Verify instructions.
    REQUIRE(program.instructionAt(a).opcode() == Opcode::FUNARG);
    REQUIRE(program.instructionAt(sum).opcode() == Opcode::ADD);
    REQUIRE(program.instructionAt(diff).opcode() == Opcode::SUB);
    REQUIRE(program.instructionAt(prod).opcode() == Opcode::MUL);

    // Print and check output.
    auto text = IRPrinter::print(program);
    REQUIRE(text.find("diff_product") != std::string::npos);
    REQUIRE(text.find("add i64") != std::string::npos);
    REQUIRE(text.find("sub i64") != std::string::npos);
    REQUIRE(text.find("mul i64") != std::string::npos);
}

TEST_CASE("IR program: conditional branch (abs value)", "[ir-programs]") {
    // Compute: abs(x) — if x < 0 then -x else x
    IRBuilder builder;
    builder.createFunction("abs", TypeId::INT32, {TypeId::INT32});

    auto x = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    auto cond = builder.emitCmpLt(TypeId::INT32, x, zero);

    auto negBB = builder.createBlock();
    auto posBB = builder.createBlock();
    auto mergeBB = builder.createBlock();
    builder.emitCondBr(cond, negBB, posBB);

    // Neg branch: return -x
    builder.setInsertPoint(negBB);
    auto negX = builder.emitNeg(TypeId::INT32, x);
    builder.emitBr(mergeBB);

    // Pos branch: return x
    builder.setInsertPoint(posBB);
    builder.emitBr(mergeBB);

    // Merge with PHI
    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{negBB, negX}, {posBB, x}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    // Verify PHI instruction
    auto phiInst = program.instructionAt(phi);
    REQUIRE(phiInst.opcode() == Opcode::PHI2);
    REQUIRE(phiInst.type() == TypeId::INT32);
    REQUIRE(phiInst.numArgs() == 4);  // 2 pairs of (block, value)

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("phi2") != std::string::npos);
    REQUIRE(text.find("cmplt") != std::string::npos);
    REQUIRE(text.find("condbr") != std::string::npos);
}

TEST_CASE("IR program: loop with PHI", "[ir-programs]") {
    // Compute: sum of 0..n-1 (using a loop)
    // sum = 0; i = 0; while (i < n) { sum += i; i++; }; return sum
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

    // Loop header: PHIs for sum and i
    builder.setInsertPoint(headerBB);
    // We'll create PHIs and patch them later
    auto phiSum = builder.emitPhi(TypeId::INT64, {{BlockId(0), initSum}, {bodyBB, INVALID_OP}});
    auto phiI = builder.emitPhi(TypeId::INT64, {{BlockId(0), initI}, {bodyBB, INVALID_OP}});
    auto cond = builder.emitCmpLt(TypeId::INT64, phiI, n);
    builder.emitCondBr(cond, bodyBB, exitBB);

    // Loop body: sum += i; i++
    builder.setInsertPoint(bodyBB);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, phiI);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    // Patch PHI back-edges
    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    // Exit
    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    // Verify program structure
    REQUIRE(program.numFunctions() == 1);
    REQUIRE(program.instructionAt(phiSum).opcode() == Opcode::PHI2);
    REQUIRE(program.instructionAt(phiI).opcode() == Opcode::PHI2);

    // After backpatching, PHI values should be patched
    auto phiSumInst = program.instructionAt(phiSum);
    REQUIRE(phiSumInst.arg(3) == newSum);  // second value = newSum

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("sum_to_n") != std::string::npos);
    REQUIRE(text.find("phi2") != std::string::npos);
    REQUIRE(text.find("cmplt") != std::string::npos);
}

TEST_CASE("IR program: memory operations", "[ir-programs]") {
    // Array sum: f(ptr, n) → sum of int64 array[0..n-1]
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

    // Header
    builder.setInsertPoint(headerBB);
    auto phiSum = builder.emitPhi(TypeId::INT64, {{BlockId(0), initSum}, {bodyBB, INVALID_OP}});
    auto phiI = builder.emitPhi(TypeId::INT64, {{BlockId(0), initI}, {bodyBB, INVALID_OP}});
    auto cond = builder.emitCmpLt(TypeId::INT64, phiI, n);
    builder.emitCondBr(cond, bodyBB, exitBB);

    // Body: load array[i], add to sum
    builder.setInsertPoint(bodyBB);
    auto elemPtr = builder.emitGEP(ptr, TypeId::INT64, phiI);
    auto val = builder.emitLoad(TypeId::INT64, elemPtr);
    auto newSum = builder.emitAdd(TypeId::INT64, phiSum, val);
    auto newI = builder.emitAdd(TypeId::INT64, phiI, one);
    builder.emitBr(headerBB);

    // Patch PHIs
    builder.patchPhiValue(phiSum, 1, newSum);
    builder.patchPhiValue(phiI, 1, newI);

    // Exit
    builder.setInsertPoint(exitBB);
    builder.emitRet(TypeId::INT64, phiSum);

    auto program = builder.finalize();

    // Verify memory instructions
    REQUIRE(program.instructionAt(elemPtr).opcode() == Opcode::GEP);
    REQUIRE(program.instructionAt(val).opcode() == Opcode::LOAD);

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("getelementptr") != std::string::npos);
    REQUIRE(text.find("load") != std::string::npos);
}

TEST_CASE("IR program: host function call", "[ir-programs]") {
    // Call a host function to compute hash
    IRBuilder builder;
    builder.createFunction("compute_hash", TypeId::UINT64, {TypeId::UINT64});

    auto arg = builder.emitFunArg(TypeId::UINT64, 0);
    auto seed = builder.emitConst(TypeId::UINT64, 0);
    auto hash = builder.emitCRC32(arg, seed);
    builder.emitRet(TypeId::UINT64, hash);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(hash).opcode() == Opcode::CRC32);

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("crc32") != std::string::npos);
}

TEST_CASE("IR program: multi-block control flow", "[ir-programs]") {
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

    // Check b > 0
    builder.setInsertPoint(checkB);
    auto condB = builder.emitCmpGt(TypeId::INT32, b, zero);
    builder.emitCondBr(condB, trueBB, falseBB);

    // True: return a + b
    builder.setInsertPoint(trueBB);
    auto sum = builder.emitAdd(TypeId::INT32, a, b);
    builder.emitBr(mergeBB);

    // False: return 0
    builder.setInsertPoint(falseBB);
    builder.emitBr(mergeBB);

    // Merge
    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{trueBB, sum}, {falseBB, zero}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    REQUIRE(program.numFunctions() == 1);
    auto text = IRPrinter::print(program);
    REQUIRE(text.find("positive_sum") != std::string::npos);
    // Should have multiple blocks
    REQUIRE(text.find("entry:") != std::string::npos);
}

TEST_CASE("IR program: select instruction", "[ir-programs]") {
    // Branchless min: f(a, b) = a < b ? a : b
    IRBuilder builder;
    builder.createFunction("branchless_min", TypeId::INT32, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);
    auto cond = builder.emitCmpLt(TypeId::INT32, a, b);
    auto result = builder.emitSelect(TypeId::INT32, cond, a, b);
    builder.emitRet(TypeId::INT32, result);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(result).opcode() == Opcode::SELECT);
    REQUIRE(program.instructionAt(result).numArgs() == 3);
}

TEST_CASE("IR program: store and load pattern", "[ir-programs]") {
    // Increment a value in memory: *ptr = *ptr + 1
    IRBuilder builder;
    builder.createFunction("increment", TypeId::VOID, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitLoad(TypeId::INT64, ptr);
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto incremented = builder.emitAdd(TypeId::INT64, val, one);
    builder.emitStore(incremented, ptr);
    builder.emitRetVoid();

    auto program = builder.finalize();

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("load i64") != std::string::npos);
    REQUIRE(text.find("store void") != std::string::npos);
}

TEST_CASE("IR program: DCE removes unreachable code", "[ir-programs]") {
    // Create dead code after an unconditional branch
    IRBuilder builder;
    builder.createFunction("test_dce", TypeId::INT32, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto retBB = builder.createBlock();
    auto deadBB = builder.createBlock();

    builder.emitBr(retBB);

    // This block is reachable
    builder.setInsertPoint(retBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    auto result = builder.emitAdd(TypeId::INT32, arg, one);
    builder.emitRet(TypeId::INT32, result);

    // This block is unreachable (no branch targets it)
    builder.setInsertPoint(deadBB);
    auto dead = builder.emitConst(TypeId::INT32, 999);
    builder.emitRet(TypeId::INT32, dead);

    auto program = builder.finalize();
    builder.eliminateDeadCode(program);

    // The dead constant 999 should be NOP'd
    auto deadInst = program.instructionAt(dead);
    REQUIRE(deadInst.opcode() == Opcode::NOP);

    // The live result should still be an ADD
    auto liveInst = program.instructionAt(result);
    REQUIRE(liveInst.opcode() == Opcode::ADD);
}

TEST_CASE("IR program: multiple comparison operators", "[ir-programs]") {
    IRBuilder builder;
    builder.createFunction("compare_all", TypeId::BOOL, {TypeId::INT32, TypeId::INT32});

    auto a = builder.emitFunArg(TypeId::INT32, 0);
    auto b = builder.emitFunArg(TypeId::INT32, 1);

    auto eq = builder.emitCmpEq(TypeId::INT32, a, b);
    auto ne = builder.emitCmpNe(TypeId::INT32, a, b);
    auto lt = builder.emitCmpLt(TypeId::INT32, a, b);
    auto le = builder.emitCmpLe(TypeId::INT32, a, b);
    auto gt = builder.emitCmpGt(TypeId::INT32, a, b);
    auto ge = builder.emitCmpGe(TypeId::INT32, a, b);

    // Combine with logical operations
    auto andResult = builder.emitLAnd(eq, ne);
    auto orResult = builder.emitLOr(lt, gt);
    auto notResult = builder.emitLNot(le);

    builder.emitRet(TypeId::BOOL, andResult);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(eq).opcode() == Opcode::CMP_EQ);
    REQUIRE(program.instructionAt(ne).opcode() == Opcode::CMP_NE);
    REQUIRE(program.instructionAt(lt).opcode() == Opcode::CMP_LT);
    REQUIRE(program.instructionAt(le).opcode() == Opcode::CMP_LE);
    REQUIRE(program.instructionAt(gt).opcode() == Opcode::CMP_GT);
    REQUIRE(program.instructionAt(ge).opcode() == Opcode::CMP_GE);
    REQUIRE(program.instructionAt(andResult).opcode() == Opcode::LAND);
    REQUIRE(program.instructionAt(orResult).opcode() == Opcode::LOR);
    REQUIRE(program.instructionAt(notResult).opcode() == Opcode::LNOT);
}

TEST_CASE("IR program: bitwise operations", "[ir-programs]") {
    IRBuilder builder;
    builder.createFunction("bitops", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto andOp = builder.emitAnd(TypeId::INT64, a, b);
    auto orOp = builder.emitOr(TypeId::INT64, a, b);
    auto xorOp = builder.emitXor(TypeId::INT64, a, b);
    auto shl = builder.emitShl(TypeId::INT64, a, b);
    auto ashr = builder.emitAShr(TypeId::INT64, a, b);
    auto lshr = builder.emitLShr(TypeId::INT64, a, b);
    auto rotl = builder.emitRotl(TypeId::INT64, a, b);
    auto rotr = builder.emitRotr(TypeId::INT64, a, b);

    builder.emitRet(TypeId::INT64, andOp);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(andOp).opcode() == Opcode::AND);
    REQUIRE(program.instructionAt(orOp).opcode() == Opcode::OR);
    REQUIRE(program.instructionAt(xorOp).opcode() == Opcode::XOR);
    REQUIRE(program.instructionAt(shl).opcode() == Opcode::SHL);
    REQUIRE(program.instructionAt(ashr).opcode() == Opcode::ASHR);
    REQUIRE(program.instructionAt(lshr).opcode() == Opcode::LSHR);
    REQUIRE(program.instructionAt(rotl).opcode() == Opcode::ROTL);
    REQUIRE(program.instructionAt(rotr).opcode() == Opcode::ROTR);
}

TEST_CASE("IR program: phi with 3 incoming edges", "[ir-programs]") {
    // Three-way branch simulation using nested if-else
    IRBuilder builder;
    builder.createFunction("three_way", TypeId::INT32, {TypeId::INT32});

    auto x = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);

    auto negBB = builder.createBlock();
    auto checkPos = builder.createBlock();
    auto posBB = builder.createBlock();
    auto zeroBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    auto isNeg = builder.emitCmpLt(TypeId::INT32, x, zero);
    builder.emitCondBr(isNeg, negBB, checkPos);

    // Negative
    builder.setInsertPoint(negBB);
    auto negOne = builder.emitConst(TypeId::INT32, static_cast<uint64_t>(-1));
    builder.emitBr(mergeBB);

    // Check positive
    builder.setInsertPoint(checkPos);
    auto isPos = builder.emitCmpGt(TypeId::INT32, x, zero);
    builder.emitCondBr(isPos, posBB, zeroBB);

    // Positive
    builder.setInsertPoint(posBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitBr(mergeBB);

    // Zero
    builder.setInsertPoint(zeroBB);
    builder.emitBr(mergeBB);

    // Merge: phi3 with 3 incoming edges
    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{negBB, negOne}, {posBB, one}, {zeroBB, zero}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();

    auto phiInst = program.instructionAt(phi);
    REQUIRE(phiInst.opcode() == Opcode::PHI3);
    REQUIRE(phiInst.numArgs() == 6);  // 3 pairs

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("phi3") != std::string::npos);
}

TEST_CASE("IR program: printer with multiple blocks", "[ir-programs]") {
    IRBuilder builder;
    builder.createFunction("multi_block", TypeId::INT32, {TypeId::INT32});

    auto x = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    auto cond = builder.emitCmpGt(TypeId::INT32, x, zero);

    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    builder.emitCondBr(cond, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitRet(TypeId::INT32, one);

    builder.setInsertPoint(falseBB);
    builder.emitRet(TypeId::INT32, zero);

    auto program = builder.finalize();
    auto text = IRPrinter::print(program);

    // Verify block labels appear
    REQUIRE(text.find("entry:") != std::string::npos);
    REQUIRE(text.find("bb") != std::string::npos);
    // Verify both return paths
    auto retCount = 0;
    std::string::size_type pos = 0;
    while ((pos = text.find("ret i32", pos)) != std::string::npos) {
        ++retCount;
        ++pos;
    }
    REQUIRE(retCount == 2);
}
