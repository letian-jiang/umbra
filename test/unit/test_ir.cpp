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

// ── Opcode metadata tests ────────────────────────────────────────────────────

TEST_CASE("Opcode metadata", "[ir]") {
    SECTION("opcode names") {
        REQUIRE(opcodeName(Opcode::CONST) == "const");
        REQUIRE(opcodeName(Opcode::ADD) == "add");
        REQUIRE(opcodeName(Opcode::SUB) == "sub");
        REQUIRE(opcodeName(Opcode::MUL) == "mul");
        REQUIRE(opcodeName(Opcode::DIV) == "div");
        REQUIRE(opcodeName(Opcode::LOAD) == "load");
        REQUIRE(opcodeName(Opcode::STORE) == "store");
        REQUIRE(opcodeName(Opcode::GEP) == "getelementptr");
        REQUIRE(opcodeName(Opcode::BR) == "br");
        REQUIRE(opcodeName(Opcode::COND_BR) == "condbr");
        REQUIRE(opcodeName(Opcode::RET) == "ret");
        REQUIRE(opcodeName(Opcode::RET_VOID) == "retvoid");
        REQUIRE(opcodeName(Opcode::PHI2) == "phi2");
        REQUIRE(opcodeName(Opcode::CRC32) == "crc32");
        REQUIRE(opcodeName(Opcode::NOP) == "nop");
    }

    SECTION("arg counts") {
        REQUIRE(opcodeArgCount(Opcode::CONST) == 0);
        REQUIRE(opcodeArgCount(Opcode::FUNARG) == 1);
        REQUIRE(opcodeArgCount(Opcode::ADD) == 2);
        REQUIRE(opcodeArgCount(Opcode::LOAD) == 1);
        REQUIRE(opcodeArgCount(Opcode::STORE) == 2);
        REQUIRE(opcodeArgCount(Opcode::BR) == 1);
        REQUIRE(opcodeArgCount(Opcode::COND_BR) == 3);
        REQUIRE(opcodeArgCount(Opcode::RET) == 1);
        REQUIRE(opcodeArgCount(Opcode::RET_VOID) == 0);
        REQUIRE(opcodeArgCount(Opcode::PHI2) == 4);
        REQUIRE(opcodeArgCount(Opcode::PHI3) == 6);
        REQUIRE(opcodeArgCount(Opcode::CALL0) == 1);
        REQUIRE(opcodeArgCount(Opcode::CALL3) == 4);
        REQUIRE(opcodeArgCount(Opcode::SELECT) == 3);
        REQUIRE(opcodeArgCount(Opcode::NOP) == 0);
    }

    SECTION("terminator check") {
        REQUIRE_FALSE(isTerminator(Opcode::ADD));
        REQUIRE_FALSE(isTerminator(Opcode::LOAD));
        REQUIRE(isTerminator(Opcode::BR));
        REQUIRE(isTerminator(Opcode::COND_BR));
        REQUIRE(isTerminator(Opcode::RET));
        REQUIRE(isTerminator(Opcode::RET_VOID));
        REQUIRE(isTerminator(Opcode::CHECKED_ADD));
    }

    SECTION("side effect check") {
        REQUIRE_FALSE(hasSideEffect(Opcode::ADD));
        REQUIRE_FALSE(hasSideEffect(Opcode::LOAD));
        REQUIRE(hasSideEffect(Opcode::STORE));
        REQUIRE(hasSideEffect(Opcode::BR));
        REQUIRE(hasSideEffect(Opcode::CALL0));
    }

    SECTION("phi helpers") {
        REQUIRE(isPhi(Opcode::PHI2));
        REQUIRE(isPhi(Opcode::PHI6));
        REQUIRE_FALSE(isPhi(Opcode::ADD));
        REQUIRE(phiIncomingCount(Opcode::PHI2) == 2);
        REQUIRE(phiIncomingCount(Opcode::PHI4) == 4);
        REQUIRE(phiOpcode(2) == Opcode::PHI2);
        REQUIRE(phiOpcode(5) == Opcode::PHI5);
    }

    SECTION("call helpers") {
        REQUIRE(isCall(Opcode::CALL0));
        REQUIRE(isCall(Opcode::CALL6));
        REQUIRE_FALSE(isCall(Opcode::ADD));
        REQUIRE(callParamCount(Opcode::CALL0) == 0);
        REQUIRE(callParamCount(Opcode::CALL3) == 3);
        REQUIRE(callOpcode(0) == Opcode::CALL0);
        REQUIRE(callOpcode(6) == Opcode::CALL6);
    }
}

// ── Type system tests ────────────────────────────────────────────────────────

TEST_CASE("IR type system", "[ir]") {
    REQUIRE(typeSize(TypeId::VOID) == 0);
    REQUIRE(typeSize(TypeId::BOOL) == 1);
    REQUIRE(typeSize(TypeId::INT8) == 1);
    REQUIRE(typeSize(TypeId::INT32) == 4);
    REQUIRE(typeSize(TypeId::INT64) == 8);
    REQUIRE(typeSize(TypeId::UINT64) == 8);
    REQUIRE(typeSize(TypeId::PTR) == 8);

    REQUIRE(typeName(TypeId::VOID) == "void");
    REQUIRE(typeName(TypeId::BOOL) == "bool");
    REQUIRE(typeName(TypeId::INT32) == "i32");
    REQUIRE(typeName(TypeId::INT64) == "i64");
    REQUIRE(typeName(TypeId::PTR) == "ptr");
}

// ── Instruction encoding/decoding tests ──────────────────────────────────────

TEST_CASE("Instruction encoding/decoding", "[ir]") {
    SECTION("encode and decode binary instruction") {
        std::vector<uint8_t> buffer;
        IROp args[] = {100, 200};
        auto offset = encodeInstruction(buffer, Opcode::ADD, TypeId::INT32, args, 2);
        REQUIRE(offset == 0);
        REQUIRE(buffer.size() == 10);  // 2 + 4*2

        InstructionView view(buffer.data(), offset);
        REQUIRE(view.opcode() == Opcode::ADD);
        REQUIRE(view.type() == TypeId::INT32);
        REQUIRE(view.numArgs() == 2);
        REQUIRE(view.arg(0) == 100);
        REQUIRE(view.arg(1) == 200);
        REQUIRE(view.offset() == 0);
        REQUIRE(view.byteSize() == 10);
    }

    SECTION("encode and decode constant") {
        std::vector<uint8_t> buffer;
        auto offset = encodeConst(buffer, TypeId::INT64, 42);
        REQUIRE(offset == 0);
        REQUIRE(buffer.size() == 10);  // 2 + 8 (payload)

        InstructionView view(buffer.data(), offset);
        REQUIRE(view.opcode() == Opcode::CONST);
        REQUIRE(view.type() == TypeId::INT64);
        REQUIRE(view.constPayload() == 42);
        REQUIRE(view.byteSize() == 10);
    }

    SECTION("constant payload sizes") {
        REQUIRE(InstructionView::constPayloadSize(TypeId::VOID) == 0);
        REQUIRE(InstructionView::constPayloadSize(TypeId::BOOL) == 1);
        REQUIRE(InstructionView::constPayloadSize(TypeId::INT8) == 1);
        REQUIRE(InstructionView::constPayloadSize(TypeId::INT32) == 4);
        REQUIRE(InstructionView::constPayloadSize(TypeId::INT64) == 8);
        REQUIRE(InstructionView::constPayloadSize(TypeId::UINT64) == 8);
        REQUIRE(InstructionView::constPayloadSize(TypeId::PTR) == 8);
    }

    SECTION("multiple instructions back to back") {
        std::vector<uint8_t> buffer;
        auto c1 = encodeConst(buffer, TypeId::INT32, 10);
        auto c2 = encodeConst(buffer, TypeId::INT32, 20);
        IROp addArgs[] = {c1, c2};
        auto addOff = encodeInstruction(buffer, Opcode::ADD, TypeId::INT32, addArgs, 2);

        InstructionView v1(buffer.data(), c1);
        REQUIRE(v1.opcode() == Opcode::CONST);
        REQUIRE(v1.constPayload() == 10);

        InstructionView v2(buffer.data(), c2);
        REQUIRE(v2.opcode() == Opcode::CONST);
        REQUIRE(v2.constPayload() == 20);

        InstructionView v3(buffer.data(), addOff);
        REQUIRE(v3.opcode() == Opcode::ADD);
        REQUIRE(v3.arg(0) == c1);
        REQUIRE(v3.arg(1) == c2);
    }

    SECTION("patch arg") {
        std::vector<uint8_t> buffer;
        IROp args[] = {0, 0, 0};
        auto offset = encodeInstruction(buffer, Opcode::COND_BR, TypeId::VOID, args, 3);
        patchArg(buffer, offset, 1, 999);

        InstructionView view(buffer.data(), offset);
        REQUIRE(view.arg(1) == 999);
        REQUIRE(view.arg(0) == 0);
        REQUIRE(view.arg(2) == 0);
    }
}

// ── Builder API tests ────────────────────────────────────────────────────────

TEST_CASE("IRBuilder basic", "[ir]") {
    SECTION("create function and emit constants") {
        IRBuilder builder;
        auto fid = builder.createFunction("test", TypeId::INT32, {});

        auto c1 = builder.emitConst(TypeId::INT32, 42);
        auto c2 = builder.emitConst(TypeId::INT32, 100);
        auto sum = builder.emitAdd(TypeId::INT32, c1, c2);
        builder.emitRet(TypeId::INT32, sum);

        auto program = builder.finalize();

        REQUIRE(program.numFunctions() == 1);
        REQUIRE(program.functionMeta(fid).name == "test");
        REQUIRE(program.functionMeta(fid).returnType == TypeId::INT32);

        // The constant should have been folded: 42 + 100 = 142
        auto inst = program.instructionAt(sum);
        REQUIRE(inst.opcode() == Opcode::CONST);
        REQUIRE(inst.constPayload() == 142);
    }

    SECTION("constant deduplication") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::VOID, {});

        auto c1 = builder.emitConst(TypeId::INT32, 42);
        auto c2 = builder.emitConst(TypeId::INT32, 42);
        REQUIRE(c1 == c2);  // Same constant → same IROp

        auto c3 = builder.emitConst(TypeId::INT32, 99);
        REQUIRE(c3 != c1);  // Different value → different IROp

        auto c4 = builder.emitConst(TypeId::INT64, 42);
        REQUIRE(c4 != c1);  // Different type → different IROp

        builder.emitRetVoid();
        auto program = builder.finalize();
        REQUIRE(program.numFunctions() == 1);
    }

    SECTION("emit function arguments") {
        IRBuilder builder;
        builder.createFunction("add2", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

        auto arg0 = builder.emitFunArg(TypeId::INT64, 0);
        auto arg1 = builder.emitFunArg(TypeId::INT64, 1);
        auto sum = builder.emitAdd(TypeId::INT64, arg0, arg1);
        builder.emitRet(TypeId::INT64, sum);

        auto program = builder.finalize();
        REQUIRE(program.functionMeta(0).numParams == 2);

        // Sum should NOT be folded (non-const operands)
        auto inst = program.instructionAt(sum);
        REQUIRE(inst.opcode() == Opcode::ADD);
    }
}

// ── Constant folding tests ───────────────────────────────────────────────────

TEST_CASE("Constant folding", "[ir]") {
    SECTION("arithmetic folding") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT32, {});

        auto a = builder.emitConst(TypeId::INT32, 10);
        auto b = builder.emitConst(TypeId::INT32, 3);

        // add(10, 3) → 13
        auto sum = builder.emitAdd(TypeId::INT32, a, b);
        // sub(10, 3) → 7
        auto diff = builder.emitSub(TypeId::INT32, a, b);
        // mul(10, 3) → 30
        auto prod = builder.emitMul(TypeId::INT32, a, b);
        // div(10, 3) → 3
        auto quot = builder.emitDiv(TypeId::INT32, a, b);

        builder.emitRet(TypeId::INT32, sum);  // Need a terminator
        auto program = builder.finalize();

        auto check = [&](IROp op, uint64_t expected) {
            auto inst = program.instructionAt(op);
            REQUIRE(inst.opcode() == Opcode::CONST);
            REQUIRE(inst.constPayload() == expected);
        };

        check(sum, 13);
        check(diff, 7);
        check(prod, 30);
        check(quot, 3);
    }

    SECTION("identity operations") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

        auto x = builder.emitFunArg(TypeId::INT64, 0);
        auto zero = builder.emitConst(TypeId::INT64, 0);
        auto one = builder.emitConst(TypeId::INT64, 1);

        // x + 0 → x
        auto addZero = builder.emitAdd(TypeId::INT64, x, zero);
        REQUIRE(addZero == x);

        // 0 + x → x
        auto zeroAdd = builder.emitAdd(TypeId::INT64, zero, x);
        REQUIRE(zeroAdd == x);

        // x - 0 → x
        auto subZero = builder.emitSub(TypeId::INT64, x, zero);
        REQUIRE(subZero == x);

        // x * 1 → x
        auto mulOne = builder.emitMul(TypeId::INT64, x, one);
        REQUIRE(mulOne == x);

        // 1 * x → x
        auto oneMul = builder.emitMul(TypeId::INT64, one, x);
        REQUIRE(oneMul == x);

        builder.emitRet(TypeId::INT64, x);
        auto program = builder.finalize();
        REQUIRE(program.numFunctions() == 1);
    }

    SECTION("absorbing operations") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

        auto x = builder.emitFunArg(TypeId::INT64, 0);
        auto zero = builder.emitConst(TypeId::INT64, 0);

        // x * 0 → 0
        auto mulZero = builder.emitMul(TypeId::INT64, x, zero);
        builder.emitRet(TypeId::INT64, mulZero);
        auto program = builder.finalize();
        auto inst = program.instructionAt(mulZero);
        REQUIRE(inst.opcode() == Opcode::CONST);
        REQUIRE(inst.constPayload() == 0);
    }

    SECTION("comparison folding") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::BOOL, {});

        auto a = builder.emitConst(TypeId::INT32, 10);
        auto b = builder.emitConst(TypeId::INT32, 20);

        auto lt = builder.emitCmpLt(TypeId::INT32, a, b);  // 10 < 20 → true
        auto gt = builder.emitCmpGt(TypeId::INT32, a, b);  // 10 > 20 → false
        auto eq = builder.emitCmpEq(TypeId::INT32, a, a);  // 10 == 10 → true

        builder.emitRet(TypeId::BOOL, lt);
        auto program = builder.finalize();

        auto checkBool = [&](IROp op, uint64_t expected) {
            auto instView = program.instructionAt(op);
            REQUIRE(instView.opcode() == Opcode::CONST);
            REQUIRE(instView.constPayload() == expected);
        };

        checkBool(lt, 1);
        checkBool(gt, 0);
        checkBool(eq, 1);
    }

    SECTION("condbr with constant condition folds to br") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::VOID, {});

        auto trueBB = builder.createBlock();
        auto falseBB = builder.createBlock();
        auto trueConst = builder.emitConst(TypeId::BOOL, 1);

        // condbr(true, trueBB, falseBB) → br trueBB
        builder.emitCondBr(trueConst, trueBB, falseBB);

        builder.setInsertPoint(trueBB);
        builder.emitRetVoid();

        builder.setInsertPoint(falseBB);
        builder.emitRetVoid();

        auto program = builder.finalize();

        // The entry block should have a BR instruction, not CONDBR.
        auto entryOff = program.functionEntry(0).entryBlockOffset;
        const auto& blockBuf = program.blockBuffer();
        // Walk to find the terminator in the entry block.
        uint32_t pos = entryOff;
        Opcode terminatorOp = Opcode::NOP;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<Opcode>(program.instBuffer()[instOff]);
            ++pos;
            if (isTerminator(opcode)) {
                terminatorOp = opcode;
                break;
            }
        }
        REQUIRE(terminatorOp == Opcode::BR);
    }

    SECTION("logical not folding") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::BOOL, {});

        auto t = builder.emitConst(TypeId::BOOL, 1);
        auto f = builder.emitConst(TypeId::BOOL, 0);

        auto notTrue = builder.emitLNot(t);   // !true → false
        auto notFalse = builder.emitLNot(f);  // !false → true

        builder.emitRet(TypeId::BOOL, notTrue);
        auto program = builder.finalize();

        REQUIRE(program.instructionAt(notTrue).constPayload() == 0);
        REQUIRE(program.instructionAt(notFalse).constPayload() == 1);
    }

    SECTION("select with constant condition") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT32, {});

        auto t = builder.emitConst(TypeId::BOOL, 1);
        auto a = builder.emitConst(TypeId::INT32, 10);
        auto b = builder.emitConst(TypeId::INT32, 20);

        auto sel = builder.emitSelect(TypeId::INT32, t, a, b);
        REQUIRE(sel == a);  // select(true, a, b) → a

        auto f = builder.emitConst(TypeId::BOOL, 0);
        auto sel2 = builder.emitSelect(TypeId::INT32, f, a, b);
        REQUIRE(sel2 == b);  // select(false, a, b) → b

        builder.emitRet(TypeId::INT32, sel);
        auto program = builder.finalize();
        REQUIRE(program.numFunctions() == 1);
    }

    SECTION("div by zero not folded") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT32, {});

        auto a = builder.emitConst(TypeId::INT32, 10);
        auto zero = builder.emitConst(TypeId::INT32, 0);

        auto result = builder.emitDiv(TypeId::INT32, a, zero);
        builder.emitRet(TypeId::INT32, result);
        auto program = builder.finalize();

        // Should NOT be folded — must remain a div instruction
        auto inst = program.instructionAt(result);
        REQUIRE(inst.opcode() == Opcode::DIV);
    }

    SECTION("bitwise and folding") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

        auto x = builder.emitFunArg(TypeId::INT64, 0);
        auto zero = builder.emitConst(TypeId::INT64, 0);

        // x & 0 → 0
        auto andZero = builder.emitAnd(TypeId::INT64, x, zero);
        builder.emitRet(TypeId::INT64, andZero);
        auto program = builder.finalize();

        auto inst = program.instructionAt(andZero);
        REQUIRE(inst.opcode() == Opcode::CONST);
        REQUIRE(inst.constPayload() == 0);
    }

    SECTION("bitwise or folding") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

        auto x = builder.emitFunArg(TypeId::INT64, 0);
        auto zero = builder.emitConst(TypeId::INT64, 0);

        // x | 0 → x
        auto orZero = builder.emitOr(TypeId::INT64, x, zero);
        REQUIRE(orZero == x);

        builder.emitRet(TypeId::INT64, x);
        auto program = builder.finalize();
        REQUIRE(program.numFunctions() == 1);
    }
}

// ── DCE tests ────────────────────────────────────────────────────────────────

TEST_CASE("Dead code elimination", "[ir]") {
    SECTION("remove unused instructions") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT32, {TypeId::INT32});

        auto arg = builder.emitFunArg(TypeId::INT32, 0);
        auto c1 = builder.emitConst(TypeId::INT32, 5);
        auto unused = builder.emitAdd(TypeId::INT32, arg, c1);  // dead: result never used
        auto c2 = builder.emitConst(TypeId::INT32, 10);
        auto used = builder.emitAdd(TypeId::INT32, arg, c2);
        builder.emitRet(TypeId::INT32, used);

        auto program = builder.finalize();
        builder.eliminateDeadCode(program);

        // The 'unused' instruction should be NOP'd
        auto inst = program.instructionAt(unused);
        REQUIRE(inst.opcode() == Opcode::NOP);

        // The 'used' instruction should still be alive
        auto instUsed = program.instructionAt(used);
        REQUIRE(instUsed.opcode() == Opcode::ADD);
    }

    SECTION("keep side-effecting instructions") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::VOID, {TypeId::PTR, TypeId::INT32});

        auto ptr = builder.emitFunArg(TypeId::PTR, 0);
        auto val = builder.emitFunArg(TypeId::INT32, 1);
        builder.emitStore(val, ptr);  // Side effect: must not be removed
        builder.emitRetVoid();

        auto program = builder.finalize();
        builder.eliminateDeadCode(program);

        // Walk the entry block and verify store is still there.
        auto entryOff = program.functionEntry(0).entryBlockOffset;
        const auto& blockBuf = program.blockBuffer();
        bool foundStore = false;
        uint32_t pos = entryOff;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<Opcode>(program.instBuffer()[instOff]);
            if (opcode == Opcode::STORE) {
                foundStore = true;
            }
            ++pos;
            if (isTerminator(opcode)) {
                break;
            }
        }
        REQUIRE(foundStore);
    }
}

// ── Backpatching tests ───────────────────────────────────────────────────────

TEST_CASE("Block backpatching", "[ir]") {
    SECTION("branch targets are backpatched") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::VOID, {});

        auto loop = builder.createBlock();
        auto exit = builder.createBlock();
        builder.emitBr(loop);

        builder.setInsertPoint(loop);
        auto cond = builder.emitConst(TypeId::BOOL, 0);
        // Force non-const condbr by using a non-const condition
        auto arg = builder.emitFunArg(TypeId::BOOL, 0);
        // Actually use the arg as condition to avoid constant folding
        builder.emitCondBr(arg, loop, exit);

        builder.setInsertPoint(exit);
        builder.emitRetVoid();

        auto program = builder.finalize();

        // Verify that branch targets are valid block buffer offsets.
        auto entryOff = program.functionEntry(0).entryBlockOffset;
        const auto& blockBuf = program.blockBuffer();
        const auto& instBuf = program.instBuffer();

        // Entry block: br → loop block offset
        // Walk to find the BR
        uint32_t pos = entryOff;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<Opcode>(instBuf[instOff]);
            ++pos;
            if (opcode == Opcode::BR) {
                InstructionView view(instBuf.data(), instOff);
                uint32_t target = view.arg(0);
                // Target should be a valid block buffer offset (not a block ID).
                REQUIRE(target < blockBuf.size());
                break;
            }
            if (isTerminator(opcode)) {
                break;
            }
        }

        (void)cond;
    }
}

// ── Printer tests ────────────────────────────────────────────────────────────

TEST_CASE("IR printer", "[ir]") {
    SECTION("print simple function") {
        IRBuilder builder;
        builder.createFunction("add_one", TypeId::INT32, {TypeId::INT32});

        auto arg = builder.emitFunArg(TypeId::INT32, 0);
        auto one = builder.emitConst(TypeId::INT32, 1);
        auto sum = builder.emitAdd(TypeId::INT32, arg, one);
        builder.emitRet(TypeId::INT32, sum);

        auto program = builder.finalize();
        auto text = IRPrinter::print(program);

        // Should contain function definition and basic instructions.
        REQUIRE(text.find("define i32 @add_one") != std::string::npos);
        REQUIRE(text.find("entry:") != std::string::npos);
        REQUIRE(text.find("funarg") != std::string::npos);
        REQUIRE(text.find("add") != std::string::npos);
        REQUIRE(text.find("ret") != std::string::npos);
    }

    SECTION("print single instruction") {
        IRBuilder builder;
        builder.createFunction("test", TypeId::INT32, {});
        auto c = builder.emitConst(TypeId::INT32, 42);
        builder.emitRet(TypeId::INT32, c);

        auto program = builder.finalize();
        auto text = IRPrinter::printInstruction(program, c);
        REQUIRE(text.find("const") != std::string::npos);
        REQUIRE(text.find("42") != std::string::npos);
    }
}

// ── Program / host function tests ────────────────────────────────────────────

TEST_CASE("Program host functions", "[ir]") {
    SECTION("register and lookup host function") {
        Program program;
        auto id = program.registerHostFunction(
            "my_hash", [](const uint64_t* args, uint8_t /*n*/) { return args[0] * 31; },
            TypeId::UINT64, {TypeId::UINT64});

        REQUIRE(program.hostFunction(id) != nullptr);
        REQUIRE(program.hostFunctionId("my_hash") == id);
        REQUIRE(program.hostFunctionId("nonexistent") == UINT32_MAX);

        // Call the host function
        uint64_t testArgs[] = {7};
        auto result = (*program.hostFunction(id))(testArgs, 1);
        REQUIRE(result == 7 * 31);
    }
}

// ── Neg instruction test ─────────────────────────────────────────────────────

TEST_CASE("Neg instruction", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT32, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto negated = builder.emitNeg(TypeId::INT32, arg);
    builder.emitRet(TypeId::INT32, negated);

    auto program = builder.finalize();

    auto inst = program.instructionAt(negated);
    REQUIRE(inst.opcode() == Opcode::NEG);
    REQUIRE(inst.numArgs() == 2);
}

// ── Cast instructions test ───────────────────────────────────────────────────

TEST_CASE("Cast instructions", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto ext = builder.emitSExt(TypeId::INT64, arg);
    auto trunc = builder.emitTrunc(TypeId::INT32, ext);
    auto zext = builder.emitZExt(TypeId::INT64, trunc);
    auto bc = builder.emitBitcast(TypeId::UINT64, zext);
    auto p2i = builder.emitPtr2Int(TypeId::UINT64, arg);
    auto i2p = builder.emitInt2Ptr(bc);
    builder.emitRet(TypeId::INT64, ext);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(ext).opcode() == Opcode::SEXT);
    REQUIRE(program.instructionAt(ext).type() == TypeId::INT64);
    REQUIRE(program.instructionAt(trunc).opcode() == Opcode::TRUNC);
    REQUIRE(program.instructionAt(zext).opcode() == Opcode::ZEXT);
    REQUIRE(program.instructionAt(bc).opcode() == Opcode::BITCAST);
    REQUIRE(program.instructionAt(p2i).opcode() == Opcode::PTR2INT);
    REQUIRE(program.instructionAt(i2p).opcode() == Opcode::INT2PTR);
    REQUIRE(program.instructionAt(i2p).type() == TypeId::PTR);
}

// ── Memory instructions test ─────────────────────────────────────────────────

TEST_CASE("Memory instructions", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto idx = builder.emitConst(TypeId::INT64, 5);
    auto gep = builder.emitGEP(ptr, TypeId::INT64, idx);
    auto loaded = builder.emitLoad(TypeId::INT64, gep);
    auto one = builder.emitConst(TypeId::INT64, 1);
    auto sum = builder.emitAdd(TypeId::INT64, loaded, one);
    builder.emitStore(sum, gep);
    builder.emitRet(TypeId::INT64, sum);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(gep).opcode() == Opcode::GEP);
    REQUIRE(program.instructionAt(gep).type() == TypeId::INT64);
    REQUIRE(program.instructionAt(loaded).opcode() == Opcode::LOAD);
}

// ── DB-specific instructions test ────────────────────────────────────────────

TEST_CASE("DB-specific instructions", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::UINT64, {TypeId::UINT64});

    auto arg = builder.emitFunArg(TypeId::UINT64, 0);
    auto isNull = builder.emitIsNull(arg);
    auto seed = builder.emitConst(TypeId::UINT64, 0);
    auto hash = builder.emitCRC32(arg, seed);
    auto swapped = builder.emitBSwap(TypeId::UINT64, arg);
    builder.emitRet(TypeId::UINT64, hash);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(isNull).opcode() == Opcode::IS_NULL);
    REQUIRE(program.instructionAt(isNull).type() == TypeId::BOOL);
    REQUIRE(program.instructionAt(hash).opcode() == Opcode::CRC32);
    REQUIRE(program.instructionAt(swapped).opcode() == Opcode::BSWAP);
}

// ── Call instruction test ────────────────────────────────────────────────────

TEST_CASE("Call instructions", "[ir]") {
    SECTION("call0: no params") {
        IRBuilder builder;
        builder.createFunction("caller", TypeId::INT64, {TypeId::INT64});
        builder.emitFunArg(TypeId::INT64, 0);
        auto call0 = builder.emitCall(TypeId::INT64, 0, {});
        builder.emitRet(TypeId::INT64, call0);
        auto prog = builder.finalize();
        REQUIRE(prog.instructionAt(call0).opcode() == Opcode::CALL0);
    }

    SECTION("call1: one param") {
        IRBuilder builder2;
        builder2.createFunction("caller2", TypeId::INT64, {TypeId::INT64});
        auto arg2 = builder2.emitFunArg(TypeId::INT64, 0);
        auto call1 = builder2.emitCall(TypeId::INT64, 0, {arg2});
        builder2.emitRet(TypeId::INT64, call1);
        auto prog2 = builder2.finalize();
        REQUIRE(prog2.instructionAt(call1).opcode() == Opcode::CALL1);
    }
}

// ── Additional coverage tests ────────────────────────────────────────────────

TEST_CASE("Constant folding: bitwise with two constants", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {});

    auto a = builder.emitConst(TypeId::INT64, 0xFF00);
    auto b = builder.emitConst(TypeId::INT64, 0x0FF0);

    // AND with two constants
    auto andResult = builder.emitAnd(TypeId::INT64, a, b);
    // OR with two constants
    auto orResult = builder.emitOr(TypeId::INT64, a, b);

    builder.emitRet(TypeId::INT64, andResult);
    auto program = builder.finalize();

    REQUIRE(program.instructionAt(andResult).opcode() == Opcode::CONST);
    REQUIRE(program.instructionAt(andResult).constPayload() == (0xFF00 & 0x0FF0));
    REQUIRE(program.instructionAt(orResult).opcode() == Opcode::CONST);
    REQUIRE(program.instructionAt(orResult).constPayload() == (0xFF00 | 0x0FF0));
}

TEST_CASE("Constant folding: identity with lhs constant", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

    auto x = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);

    // 0 + x → x
    auto zeroAddX = builder.emitAdd(TypeId::INT64, zero, x);
    REQUIRE(zeroAddX == x);

    // 0 * x → 0
    auto zeroMulX = builder.emitMul(TypeId::INT64, zero, x);
    auto program1 = builder.finalize();
    REQUIRE(program1.instructionAt(zeroMulX).opcode() == Opcode::CONST);
    REQUIRE(program1.instructionAt(zeroMulX).constPayload() == 0);
}

TEST_CASE("Constant folding: AND identity lhs zero", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

    auto x = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);

    // 0 & x → 0
    auto zeroAndX = builder.emitAnd(TypeId::INT64, zero, x);
    builder.emitRet(TypeId::INT64, zeroAndX);
    auto program = builder.finalize();
    REQUIRE(program.instructionAt(zeroAndX).opcode() == Opcode::CONST);
    REQUIRE(program.instructionAt(zeroAndX).constPayload() == 0);
}

TEST_CASE("Constant folding: OR identity lhs zero", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64});

    auto x = builder.emitFunArg(TypeId::INT64, 0);
    auto zero = builder.emitConst(TypeId::INT64, 0);

    // 0 | x → x
    auto zeroOrX = builder.emitOr(TypeId::INT64, zero, x);
    REQUIRE(zeroOrX == x);

    builder.emitRet(TypeId::INT64, x);
    auto program = builder.finalize();
    REQUIRE(program.numFunctions() == 1);
}

TEST_CASE("Constant folding: negative values", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {});

    auto a = builder.emitConst(TypeId::INT64, static_cast<uint64_t>(-5));
    auto b = builder.emitConst(TypeId::INT64, static_cast<uint64_t>(-3));
    auto sum = builder.emitAdd(TypeId::INT64, a, b);
    builder.emitRet(TypeId::INT64, sum);
    auto program = builder.finalize();

    REQUIRE(program.instructionAt(sum).opcode() == Opcode::CONST);
    REQUIRE(static_cast<int64_t>(program.instructionAt(sum).constPayload()) == -8);
}

TEST_CASE("Constant comparison: unsigned", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::BOOL, {});

    auto a = builder.emitConst(TypeId::UINT64, 5);
    auto b = builder.emitConst(TypeId::UINT64, 10);

    auto lt = builder.emitCmpLt(TypeId::UINT64, a, b);
    auto ge = builder.emitCmpGe(TypeId::UINT64, a, b);
    auto le = builder.emitCmpLe(TypeId::UINT64, a, b);
    auto ne = builder.emitCmpNe(TypeId::UINT64, a, b);
    auto gt = builder.emitCmpGt(TypeId::UINT64, a, b);

    builder.emitRet(TypeId::BOOL, lt);
    auto program = builder.finalize();

    REQUIRE(program.instructionAt(lt).constPayload() == 1);
    REQUIRE(program.instructionAt(ge).constPayload() == 0);
    REQUIRE(program.instructionAt(le).constPayload() == 1);
    REQUIRE(program.instructionAt(ne).constPayload() == 1);
    REQUIRE(program.instructionAt(gt).constPayload() == 0);
}

TEST_CASE("Bool and Int8 constants", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::BOOL, {});

    auto t = builder.emitConst(TypeId::BOOL, 1);
    auto f = builder.emitConst(TypeId::BOOL, 0);
    auto i8 = builder.emitConst(TypeId::INT8, 42);

    builder.emitRet(TypeId::BOOL, t);
    auto program = builder.finalize();

    REQUIRE(program.instructionAt(t).type() == TypeId::BOOL);
    REQUIRE(program.instructionAt(t).constPayload() == 1);
    REQUIRE(program.instructionAt(f).constPayload() == 0);
    REQUIRE(program.instructionAt(i8).type() == TypeId::INT8);
    REQUIRE(program.instructionAt(i8).constPayload() == 42);
}

TEST_CASE("Printer with host function", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::VOID, {});
    builder.emitRetVoid();
    auto program = builder.finalize();

    // Register a host function
    program.registerHostFunction(
        "hash_fn", [](const uint64_t* /*args*/, uint8_t /*n*/) -> uint64_t { return 0; },
        TypeId::UINT64, {TypeId::UINT64});

    auto text = IRPrinter::print(program);
    REQUIRE(text.find("host function @hash_fn") != std::string::npos);
}

TEST_CASE("Printer with PHI and branches", "[ir]") {
    IRBuilder builder;
    builder.createFunction("phi_test", TypeId::INT32, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    auto cond = builder.emitCmpGt(TypeId::INT32, arg, zero);

    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    builder.emitCondBr(cond, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(falseBB);
    auto negOne = builder.emitConst(TypeId::INT32, static_cast<uint64_t>(-1));
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{trueBB, one}, {falseBB, negOne}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();
    auto text = IRPrinter::printFunction(program, 0);

    REQUIRE(text.find("define i32 @phi_test") != std::string::npos);
    REQUIRE(text.find("condbr") != std::string::npos);
    REQUIRE(text.find("phi2") != std::string::npos);
    REQUIRE(text.find("ret i32") != std::string::npos);
}

TEST_CASE("DCE with empty program", "[ir]") {
    IRBuilder builder;
    auto program = builder.finalize();
    builder.eliminateDeadCode(program);
    // Should not crash
    REQUIRE(program.instBuffer().empty());
}

TEST_CASE("Checked arithmetic instructions", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();

    auto result = builder.emitCheckedAdd(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT64, result);

    builder.setInsertPoint(overflowBB);
    auto errVal = builder.emitConst(TypeId::INT64, static_cast<uint64_t>(-1));
    builder.emitRet(TypeId::INT64, errVal);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(result).opcode() == Opcode::CHECKED_ADD);
    REQUIRE(program.instructionAt(result).numArgs() == 4);
}

TEST_CASE("Checked sub and mul", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();
    auto contBB2 = builder.createBlock();
    auto overflowBB2 = builder.createBlock();

    auto subResult = builder.emitCheckedSub(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    auto mulResult = builder.emitCheckedMul(TypeId::INT64, subResult, b, contBB2, overflowBB2);

    builder.setInsertPoint(contBB2);
    builder.emitRet(TypeId::INT64, mulResult);

    builder.setInsertPoint(overflowBB);
    auto zero = builder.emitConst(TypeId::INT64, 0);
    builder.emitRet(TypeId::INT64, zero);

    builder.setInsertPoint(overflowBB2);
    auto zero2 = builder.emitConst(TypeId::INT64, 0);
    builder.emitRet(TypeId::INT64, zero2);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(subResult).opcode() == Opcode::CHECKED_SUB);
    REQUIRE(program.instructionAt(mulResult).opcode() == Opcode::CHECKED_MUL);
}

TEST_CASE("UDiv and Mod instructions", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::UINT64, {TypeId::UINT64, TypeId::UINT64});

    auto a = builder.emitFunArg(TypeId::UINT64, 0);
    auto b = builder.emitFunArg(TypeId::UINT64, 1);

    auto udivResult = builder.emitUDiv(TypeId::UINT64, a, b);
    auto modResult = builder.emitMod(TypeId::UINT64, a, b);
    builder.emitRet(TypeId::UINT64, udivResult);

    auto program = builder.finalize();

    REQUIRE(program.instructionAt(udivResult).opcode() == Opcode::UDIV);
    REQUIRE(program.instructionAt(modResult).opcode() == Opcode::MOD);
}

TEST_CASE("Constant dedup with different 64-bit values", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {});

    auto a = builder.emitConst(TypeId::INT64, 123456789012345);
    auto b = builder.emitConst(TypeId::INT64, 123456789012345);
    REQUIRE(a == b);  // Same value → same IROp

    auto c = builder.emitConst(TypeId::INT64, 987654321098765);
    REQUIRE(c != a);  // Different value

    auto d = builder.emitConst(TypeId::PTR, 0x12345678);
    auto e = builder.emitConst(TypeId::PTR, 0x12345678);
    REQUIRE(d == e);  // Same ptr value → same IROp

    builder.emitRet(TypeId::INT64, a);
    auto program = builder.finalize();
    REQUIRE(program.numFunctions() == 1);
}

TEST_CASE("Phi block patching", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT32, {TypeId::INT32});

    auto arg = builder.emitFunArg(TypeId::INT32, 0);
    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    auto mergeBB = builder.createBlock();

    auto cond = builder.emitCmpGt(TypeId::INT32, arg, builder.emitConst(TypeId::INT32, 0));
    builder.emitCondBr(cond, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    auto one = builder.emitConst(TypeId::INT32, 1);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(falseBB);
    auto zero = builder.emitConst(TypeId::INT32, 0);
    builder.emitBr(mergeBB);

    builder.setInsertPoint(mergeBB);
    auto phi = builder.emitPhi(TypeId::INT32, {{trueBB, one}, {falseBB, zero}});
    builder.emitRet(TypeId::INT32, phi);

    auto program = builder.finalize();
    auto phiInst = program.instructionAt(phi);
    REQUIRE(phiInst.opcode() == Opcode::PHI2);
    // Values should be the actual IROps (not block IDs)
    REQUIRE(phiInst.arg(1) == one);
    REQUIRE(phiInst.arg(3) == zero);
}

TEST_CASE("Condbr with false constant", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::VOID, {});

    auto trueBB = builder.createBlock();
    auto falseBB = builder.createBlock();
    auto falseConst = builder.emitConst(TypeId::BOOL, 0);

    // condbr(false, trueBB, falseBB) → br falseBB
    builder.emitCondBr(falseConst, trueBB, falseBB);

    builder.setInsertPoint(trueBB);
    builder.emitRetVoid();

    builder.setInsertPoint(falseBB);
    builder.emitRetVoid();

    auto program = builder.finalize();

    // The entry block should have a BR instruction (not CONDBR)
    auto entryOff = program.functionEntry(0).entryBlockOffset;
    const auto& blockBuf = program.blockBuffer();
    uint32_t pos = entryOff;
    Opcode terminatorOp = Opcode::NOP;
    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        auto opcode = static_cast<Opcode>(program.instBuffer()[instOff]);
        ++pos;
        if (isTerminator(opcode)) {
            terminatorOp = opcode;
            break;
        }
    }
    REQUIRE(terminatorOp == Opcode::BR);
}

TEST_CASE("RetVoid instruction", "[ir]") {
    IRBuilder builder;
    builder.createFunction("void_func", TypeId::VOID, {});
    builder.emitRetVoid();

    auto program = builder.finalize();
    auto text = IRPrinter::print(program);
    REQUIRE(text.find("retvoid") != std::string::npos);
}

TEST_CASE("Opcode type names", "[ir]") {
    REQUIRE(typeName(TypeId::INT8) == "i8");
    REQUIRE(typeName(TypeId::UINT64) == "u64");
}

TEST_CASE("Printer: constant types", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::VOID, {});

    // Cover all constant type printing paths
    auto boolConst = builder.emitConst(TypeId::BOOL, 1);
    auto int8Const = builder.emitConst(TypeId::INT8, 42);
    auto int32Const = builder.emitConst(TypeId::INT32, 100);
    auto int64Const = builder.emitConst(TypeId::INT64, 9999);
    auto uint64Const = builder.emitConst(TypeId::UINT64, 12345);
    auto ptrConst = builder.emitConst(TypeId::PTR, 0xDEAD);

    builder.emitRetVoid();
    auto program = builder.finalize();

    // Print each constant instruction individually
    auto boolText = IRPrinter::printInstruction(program, boolConst);
    REQUIRE(boolText.find("true") != std::string::npos);

    auto int8Text = IRPrinter::printInstruction(program, int8Const);
    REQUIRE(int8Text.find("42") != std::string::npos);

    auto ptrText = IRPrinter::printInstruction(program, ptrConst);
    REQUIRE(ptrText.find("0x") != std::string::npos);

    auto uint64Text = IRPrinter::printInstruction(program, uint64Const);
    REQUIRE(uint64Text.find("12345") != std::string::npos);

    auto int64Text = IRPrinter::printInstruction(program, int64Const);
    REQUIRE(int64Text.find("9999") != std::string::npos);

    auto int32Text = IRPrinter::printInstruction(program, int32Const);
    REQUIRE(int32Text.find("100") != std::string::npos);

    // Also print false bool
    auto falseConst = builder.emitConst(TypeId::BOOL, 0);
    // Need a new builder since we already finalized
    IRBuilder builder2;
    builder2.createFunction("test2", TypeId::VOID, {});
    auto fc = builder2.emitConst(TypeId::BOOL, 0);
    builder2.emitRetVoid();
    auto prog2 = builder2.finalize();
    auto falseText = IRPrinter::printInstruction(prog2, fc);
    REQUIRE(falseText.find("false") != std::string::npos);

    (void)boolConst;
    (void)falseConst;
}

TEST_CASE("Printer: checked arithmetic blocks", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::INT64, {TypeId::INT64, TypeId::INT64});

    auto a = builder.emitFunArg(TypeId::INT64, 0);
    auto b = builder.emitFunArg(TypeId::INT64, 1);

    auto contBB = builder.createBlock();
    auto overflowBB = builder.createBlock();

    auto result = builder.emitCheckedAdd(TypeId::INT64, a, b, contBB, overflowBB);

    builder.setInsertPoint(contBB);
    builder.emitRet(TypeId::INT64, result);

    builder.setInsertPoint(overflowBB);
    auto errVal = builder.emitConst(TypeId::INT64, static_cast<uint64_t>(-1));
    builder.emitRet(TypeId::INT64, errVal);

    auto program = builder.finalize();
    auto text = IRPrinter::print(program);

    // Should print the checked add and both blocks
    REQUIRE(text.find("checkedadd") != std::string::npos);
    REQUIRE(text.find("entry:") != std::string::npos);
}

TEST_CASE("Printer: print instruction with various types", "[ir]") {
    IRBuilder builder;
    builder.createFunction("test", TypeId::VOID, {TypeId::PTR});

    auto ptr = builder.emitFunArg(TypeId::PTR, 0);
    auto val = builder.emitLoad(TypeId::INT32, ptr);
    builder.emitStore(val, ptr);
    builder.emitRetVoid();

    auto program = builder.finalize();
    auto text = IRPrinter::print(program);

    REQUIRE(text.find("load i32") != std::string::npos);
    REQUIRE(text.find("store void") != std::string::npos);
    REQUIRE(text.find("retvoid") != std::string::npos);
}
