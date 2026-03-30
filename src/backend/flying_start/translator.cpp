#include "backend/flying_start/translator.h"

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "backend/flying_start/live_span.h"
#include "backend/flying_start/optimizations.h"
#include "backend/flying_start/register_alloc.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

namespace x86 = asmjit::x86;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace {

/// Collect instruction offsets from a block.
std::vector<uint32_t> collectBlockInsts(const ir::Program& program, uint32_t blockOff) {
    std::vector<uint32_t> insts;
    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();

    uint32_t pos = blockOff;
    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        insts.push_back(instOff);

        auto opcode = static_cast<ir::Opcode>(instBuf[instOff]);
        ++pos;
        if (ir::isTerminator(opcode)) {
            break;
        }
    }
    return insts;
}

/// Wrapper to call a host function from JIT code.
extern "C" uint64_t jitCallHostFunction(const void* funcVoid, const uint64_t* args,
                                        uint64_t count) {
    const auto* func = static_cast<const ir::HostFunction*>(funcVoid);
    return (*func)(args, static_cast<uint8_t>(count));
}

uint8_t gepScaleShift(ir::TypeId elemType) {
    switch (ir::typeSize(elemType)) {
        case 1:
            return 0;
        case 4:
            return 2;
        case 8:
            return 3;
        default:
            return 0;
    }
}

}  // namespace

// ── Constructor / Destructor ─────────────────────────────────────────────────

FlyingStartCompiler::FlyingStartCompiler() = default;

FlyingStartCompiler::~FlyingStartCompiler() = default;

// ── Public API ───────────────────────────────────────────────────────────────

void* FlyingStartCompiler::compile(ir::Program& program, ir::FunctionId func) {
    currentProgram_ = &program;

    // Pass 0: Live span analysis.
    lsa_.analyze(program, func);

    // Reset state.
    regAlloc_ = RegisterAllocator();
    blockLabels_.clear();

    code_ = new asmjit::CodeHolder();
    code_->init(runtime_.environment());
    as_ = new x86::Assembler(code_);

    for (uint32_t blockOff : lsa_.rpoOrder()) {
        blockLabels_[blockOff] = as_->new_label();
    }
    epilogueLabel_ = as_->new_label();

    const auto& meta = program.functionMeta(func);
    numParams_ = meta.numParams;

    // Translate function. Stack slots are pre-allocated for value-producing
    // instructions and freed via live span analysis for same-block reuse.
    translateFunction(program, func);

    void* funcPtr = nullptr;
    runtime_.add(&funcPtr, code_);

    delete as_;
    as_ = nullptr;
    delete code_;
    code_ = nullptr;

    return funcPtr;
}

void* FlyingStartCompiler::compileWithLog(ir::Program& program, ir::FunctionId func,
                                          std::string& asmOutput) {
    currentProgram_ = &program;

    // Pass 0: Live span analysis.
    lsa_.analyze(program, func);

    // Reset state.
    regAlloc_ = RegisterAllocator();
    blockLabels_.clear();

    code_ = new asmjit::CodeHolder();
    code_->init(runtime_.environment());

    // Attach a StringLogger to capture assembly output.
    asmjit::StringLogger logger;
    code_->set_logger(&logger);

    as_ = new x86::Assembler(code_);

    for (uint32_t blockOff : lsa_.rpoOrder()) {
        blockLabels_[blockOff] = as_->new_label();
    }
    epilogueLabel_ = as_->new_label();

    const auto& meta = program.functionMeta(func);
    numParams_ = meta.numParams;

    // Stack slots are allocated on-demand during translation.

    // Translate function.
    translateFunction(program, func);

    void* funcPtr = nullptr;
    runtime_.add(&funcPtr, code_);

    // Capture the assembly text.
    asmOutput.append(logger.data(), logger.data_size());

    delete as_;
    as_ = nullptr;
    delete code_;
    code_ = nullptr;

    return funcPtr;
}

uint64_t FlyingStartCompiler::execute(void* funcPtr, std::span<const uint64_t> args) {
    switch (args.size()) {
        case 0: {
            using FnPtr = uint64_t (*)();
            return reinterpret_cast<FnPtr>(funcPtr)();  // NOLINT
        }
        case 1: {
            using FnPtr = uint64_t (*)(uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0]);  // NOLINT
        }
        case 2: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1]);  // NOLINT
        }
        case 3: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2]);  // NOLINT
        }
        case 4: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2],  // NOLINT
                                                    args[3]);
        }
        case 5: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2],  // NOLINT
                                                    args[3], args[4]);
        }
        case 6: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2],  // NOLINT
                                                    args[3], args[4], args[5]);
        }
        default:
            assert(false && "Too many arguments");
            return 0;
    }
}

uint64_t FlyingStartCompiler::compileAndExecute(ir::Program& program, ir::FunctionId func,
                                                std::span<const uint64_t> args) {
    void* funcPtr = compile(program, func);
    return execute(funcPtr, args);
}

// ── Slot access ──────────────────────────────────────────────────────────────

asmjit::x86::Gp FlyingStartCompiler::loadValueToReg(ir::IROp op, const asmjit::x86::Gp& hint) {
    const auto* loc = regAlloc_.getLocation(op);
    if (loc == nullptr) {
        as_->xor_(hint, hint);
        return hint;
    }

    switch (loc->kind) {
        case LocationKind::STACK:
            as_->mov(hint, x86::qword_ptr(x86::rbp, loc->stackOffset));
            return hint;
        case LocationKind::CONST:
            if (loc->constValue == 0) {
                as_->xor_(hint, hint);
            } else {
                as_->mov(hint, loc->constValue);
            }
            return hint;
        case LocationKind::REGISTER:
            if (hint.id() != loc->reg.id()) {
                as_->mov(hint, loc->reg);
            }
            return hint;
        default:
            as_->xor_(hint, hint);
            return hint;
    }
}

asmjit::x86::Gp FlyingStartCompiler::loadValueForModify(ir::IROp op, const asmjit::x86::Gp& hint) {
    return loadValueToReg(op, hint);
}

void FlyingStartCompiler::storeResult(uint32_t instOff, const asmjit::x86::Gp& reg,
                                      ir::TypeId type) {
    auto loc = assignResultLocation(instOff, type);
    if (loc.kind == LocationKind::STACK) {
        as_->mov(x86::qword_ptr(x86::rbp, loc.stackOffset), reg);
    }
}

ValueLocation FlyingStartCompiler::assignResultLocation(uint32_t instOff, ir::TypeId type) {
    // Check if already assigned (from pre-allocation).
    const auto* existing = regAlloc_.getLocation(instOff);
    if (existing != nullptr) {
        return *existing;
    }
    // Fallback: allocate new stack slot.
    ValueLocation loc;
    loc.kind = LocationKind::STACK;
    loc.stackOffset = regAlloc_.allocateStackSlot();
    loc.type = type;
    regAlloc_.setLocation(instOff, loc);
    return loc;
}

void FlyingStartCompiler::emitComparison(ir::IROp lhsOp, ir::IROp rhsOp) {
    auto lhsReg = loadValueToReg(lhsOp, x86::rax);
    auto rhsReg = loadValueToReg(rhsOp, x86::rcx);
    as_->cmp(lhsReg, rhsReg);
}

bool FlyingStartCompiler::isConstOp(const ir::Program& program, ir::IROp op) {
    return op < program.instBuffer().size() &&
           static_cast<ir::Opcode>(program.instBuffer()[op]) == ir::Opcode::CONST;
}

uint64_t FlyingStartCompiler::getConstValue(const ir::Program& program, ir::IROp op) {
    ir::InstructionView view(program.instBuffer().data(), op);
    return view.constPayload();
}

FlyingStartCompiler::SignedMagic FlyingStartCompiler::computeSignedMagic(int64_t divisor) {
    assert(divisor > 1);

    const uint64_t ad = static_cast<uint64_t>(divisor);
    const uint64_t two63 = uint64_t{1} << 63;
    const uint64_t t = two63;
    const uint64_t anc = t - 1 - (t % ad);

    uint64_t p = 63;
    uint64_t q1 = two63 / anc;
    uint64_t r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / ad;
    uint64_t r2 = two63 - q2 * ad;

    uint64_t delta = 0;
    do {
        ++p;

        q1 <<= 1;
        r1 <<= 1;
        if (r1 >= anc) {
            ++q1;
            r1 -= anc;
        }

        q2 <<= 1;
        r2 <<= 1;
        if (r2 >= ad) {
            ++q2;
            r2 -= ad;
        }

        delta = ad - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));

    return {
        .magic = static_cast<int64_t>(q2 + 1),
        .shift = static_cast<uint8_t>(p - 64),
    };
}

void FlyingStartCompiler::freeIfLastUse(ir::IROp /*op*/, uint32_t /*seqNum*/) {
    // Stack space reuse is performed in the pre-allocation pass
    // (translateFunction), not during code emission.
}

// ── Translation ──────────────────────────────────────────────────────────────

void FlyingStartCompiler::translateFunction(ir::Program& program, ir::FunctionId /*func*/) {
    // Pre-allocate stack slots with integrated reuse: traverse all instructions
    // in RPO order, freeing same-block values at their last use so that later
    // instructions reuse the freed slots. This ensures every value has a slot
    // before code emission while reducing the stack frame high-water mark.
    {
        uint32_t preSeq = 0;
        for (uint32_t blockOff : lsa_.rpoOrder()) {
            auto insts = collectBlockInsts(program, blockOff);
            for (uint32_t instOff : insts) {
                ir::InstructionView inst(program.instBuffer().data(), instOff);
                auto opcode = inst.opcode();

                // Return same-block value slots to the free pool at their last use,
                // enabling subsequent allocations to reuse them. The location MAP
                // entry is kept so code emission finds the correct slot offset.
                ir::forEachValueArg(inst, [&](ir::IROp op) {
                    if (lsa_.isLastUse(op, preSeq) && lsa_.blockOf(op) == blockOff) {
                        const auto* loc = regAlloc_.getLocation(op);
                        if (loc != nullptr && loc->kind == LocationKind::STACK) {
                            regAlloc_.freeStackSlot(loc->stackOffset);
                        }
                    }
                });

                // Allocate slot for value-producing instructions.
                if (!ir::isTerminator(opcode) && opcode != ir::Opcode::STORE &&
                    opcode != ir::Opcode::NOP) {
                    assignResultLocation(instOff, inst.type());
                }
                ++preSeq;
            }
        }
    }

    emitPrologue();

    currentSeq_ = 0;
    for (uint32_t blockOff : lsa_.rpoOrder()) {
        translateBlock(program, blockOff);
    }

    emitEpilogue();
}

void FlyingStartCompiler::translateBlock(ir::Program& program, uint32_t blockOff) {
    currentBlockOff_ = blockOff;
    as_->bind(blockLabels_[blockOff]);

    auto insts = collectBlockInsts(program, blockOff);

    for (uint32_t instOff : insts) {
        ir::InstructionView inst(program.instBuffer().data(), instOff);

        if (ir::isPhi(inst.opcode())) {
            // Allocate a stack slot for the PHI result (on-demand).
            assignResultLocation(instOff, inst.type());
            ++currentSeq_;
            continue;
        }

        if (inst.opcode() == ir::Opcode::NOP) {
            ++currentSeq_;
            continue;
        }

        translateInstruction(program, inst, instOff);
        freeLastUseOperands(inst, instOff);
        ++currentSeq_;
    }
}

void FlyingStartCompiler::translateInstruction(ir::Program& program,
                                               const ir::InstructionView& inst, uint32_t instOff) {
    using ir::Opcode;

    switch (inst.opcode()) {
        case Opcode::CONST:
            translateConst(inst, instOff);
            break;
        case Opcode::FUNARG:
            translateFunArg(inst, instOff);
            break;
        case Opcode::ADD:
            translateAdd(program, inst, instOff);
            break;
        case Opcode::SUB:
            translateSub(program, inst, instOff);
            break;
        case Opcode::MUL:
            translateMul(program, inst, instOff);
            break;
        case Opcode::DIV:
            translateDiv(program, inst, instOff);
            break;
        case Opcode::UDIV:
            translateUDiv(program, inst, instOff);
            break;
        case Opcode::MOD:
            translateMod(program, inst, instOff);
            break;
        case Opcode::NEG:
            translateNeg(program, inst, instOff);
            break;
        case Opcode::CHECKED_ADD:
        case Opcode::CHECKED_SUB:
        case Opcode::CHECKED_MUL:
            translateCheckedArith(program, inst, instOff);
            break;
        case Opcode::AND:
            translateBitwiseAnd(program, inst, instOff);
            break;
        case Opcode::OR:
            translateBitwiseOr(program, inst, instOff);
            break;
        case Opcode::XOR:
            translateBitwiseXor(program, inst, instOff);
            break;
        case Opcode::SHL:
            translateShl(program, inst, instOff);
            break;
        case Opcode::ASHR:
            translateAShr(program, inst, instOff);
            break;
        case Opcode::LSHR:
            translateLShr(program, inst, instOff);
            break;
        case Opcode::ROTL:
            translateRotl(program, inst, instOff);
            break;
        case Opcode::ROTR:
            translateRotr(program, inst, instOff);
            break;
        case Opcode::CMP_EQ:
        case Opcode::CMP_NE:
        case Opcode::CMP_LT:
        case Opcode::CMP_LE:
        case Opcode::CMP_GT:
        case Opcode::CMP_GE:
            translateCmp(program, inst, instOff);
            break;
        case Opcode::LNOT:
            translateLNot(program, inst, instOff);
            break;
        case Opcode::LAND:
            translateLAnd(program, inst, instOff);
            break;
        case Opcode::LOR:
            translateLOr(program, inst, instOff);
            break;
        case Opcode::SELECT:
            translateSelect(program, inst, instOff);
            break;
        case Opcode::SEXT:
            translateSExt(program, inst, instOff);
            break;
        case Opcode::ZEXT:
            translateZExt(program, inst, instOff);
            break;
        case Opcode::TRUNC:
            translateTrunc(program, inst, instOff);
            break;
        case Opcode::BITCAST:
        case Opcode::PTR2INT:
        case Opcode::INT2PTR:
            translateBitcast(program, inst, instOff);
            break;
        case Opcode::LOAD:
            translateLoad(program, inst, instOff);
            break;
        case Opcode::STORE:
            translateStore(program, inst, instOff);
            break;
        case Opcode::GEP:
            translateGep(program, inst, instOff);
            break;
        case Opcode::BR:
            translateBr(program, inst);
            break;
        case Opcode::COND_BR:
            translateCondBr(program, inst);
            break;
        case Opcode::RET:
            translateRet(program, inst);
            break;
        case Opcode::RET_VOID:
            translateRetVoid();
            break;
        case Opcode::CALL0:
        case Opcode::CALL1:
        case Opcode::CALL2:
        case Opcode::CALL3:
        case Opcode::CALL4:
        case Opcode::CALL5:
        case Opcode::CALL6:
            translateCall(program, inst, instOff);
            break;
        case Opcode::IS_NULL:
            translateIsNull(program, inst, instOff);
            break;
        case Opcode::CRC32:
            translateCrc32(program, inst, instOff);
            break;
        case Opcode::BSWAP:
            translateBSwap(program, inst, instOff);
            break;
        case Opcode::SWITCH:
        case Opcode::NOP:
        case Opcode::NUM_OPCODES:
        case Opcode::PHI2:
        case Opcode::PHI3:
        case Opcode::PHI4:
        case Opcode::PHI5:
        case Opcode::PHI6:
            break;
    }
}

// ── Prologue / Epilogue ──────────────────────────────────────────────────────

void FlyingStartCompiler::emitPrologue() {
    as_->push(x86::rbp);
    as_->mov(x86::rbp, x86::rsp);

    // Reserve a generous frame size. We'll use up to 4096 bytes (512 slots).
    // This avoids the complexity of backpatching.
    as_->sub(x86::rsp, 4096);

    // Save callee-saved registers.
    as_->push(x86::rbx);
    as_->push(x86::r12);
    as_->push(x86::r13);
    as_->push(x86::r14);
    as_->push(x86::r15);
}

void FlyingStartCompiler::emitEpilogue() {
    as_->bind(epilogueLabel_);

    as_->pop(x86::r15);
    as_->pop(x86::r14);
    as_->pop(x86::r13);
    as_->pop(x86::r12);
    as_->pop(x86::rbx);

    as_->mov(x86::rsp, x86::rbp);
    as_->pop(x86::rbp);
    as_->ret();
}

// ── Opcode translations ──────────────────────────────────────────────────────
// All translations use RAX, RCX, RDX, RSI as scratch registers.
// Values are read from stack slots and results written back to stack slots.

void FlyingStartCompiler::translateConst(const ir::InstructionView& inst, uint32_t instOff) {
    uint64_t val = inst.constPayload();
    if (val == 0) {
        as_->xor_(x86::rax, x86::rax);
    } else {
        as_->mov(x86::rax, val);
    }
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateFunArg(const ir::InstructionView& inst, uint32_t instOff) {
    // System V ABI: rdi, rsi, rdx, rcx, r8, r9
    // NOLINTNEXTLINE(modernize-avoid-c-arrays,readability-identifier-naming)
    static const std::array<x86::Gp, 6> ABI_REGS = {x86::rdi, x86::rsi, x86::rdx,
                                                    x86::rcx, x86::r8,  x86::r9};
    uint32_t argIdx = inst.arg(0);

    if (argIdx < 6) {
        storeResult(instOff, ABI_REGS[argIdx], inst.type());
    }
}

void FlyingStartCompiler::translateAdd(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->add(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateSub(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->sub(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateMul(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->imul(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateDiv(ir::Program& program, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);

    if ((inst.type() == ir::TypeId::INT32 || inst.type() == ir::TypeId::INT64) &&
        isConstOp(program, inst.arg(1))) {
        int64_t divisor = static_cast<int64_t>(getConstValue(program, inst.arg(1)));
        if (divisor > 1) {
            if (inst.type() == ir::TypeId::INT32) {
                as_->movsxd(x86::rax, x86::eax);
            }

            const auto magic = computeSignedMagic(divisor);
            as_->mov(x86::rsi, x86::rax);
            as_->mov(x86::rcx, static_cast<uint64_t>(magic.magic));
            as_->imul(x86::rcx);

            if (magic.magic < 0) {
                as_->add(x86::rdx, x86::rsi);
            }
            if (magic.shift != 0) {
                as_->sar(x86::rdx, magic.shift);
            }

            as_->mov(x86::rax, x86::rdx);
            as_->shr(x86::rax, 63);
            as_->add(x86::rdx, x86::rax);
            storeResult(instOff, x86::rdx, inst.type());
            return;
        }
    }

    loadValueToReg(inst.arg(1), x86::rcx);

    auto nonZeroLabel = as_->new_label();
    auto doneLabel = as_->new_label();

    as_->test(x86::rcx, x86::rcx);
    as_->jnz(nonZeroLabel);
    as_->xor_(x86::rax, x86::rax);
    as_->jmp(doneLabel);

    as_->bind(nonZeroLabel);
    if (inst.type() == ir::TypeId::INT32) {
        as_->movsxd(x86::rax, x86::eax);
    }
    as_->cqo();
    as_->idiv(x86::rcx);

    as_->bind(doneLabel);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateUDiv(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);

    auto nonZeroLabel = as_->new_label();
    auto doneLabel = as_->new_label();

    as_->test(x86::rcx, x86::rcx);
    as_->jnz(nonZeroLabel);
    as_->xor_(x86::rax, x86::rax);
    as_->jmp(doneLabel);

    as_->bind(nonZeroLabel);
    as_->xor_(x86::rdx, x86::rdx);
    as_->div(x86::rcx);

    as_->bind(doneLabel);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateMod(ir::Program& program, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);

    if ((inst.type() == ir::TypeId::INT32 || inst.type() == ir::TypeId::INT64) &&
        isConstOp(program, inst.arg(1))) {
        int64_t divisor = static_cast<int64_t>(getConstValue(program, inst.arg(1)));
        if (divisor > 1) {
            if (inst.type() == ir::TypeId::INT32) {
                as_->movsxd(x86::rax, x86::eax);
            }

            const auto magic = computeSignedMagic(divisor);
            as_->mov(x86::rsi, x86::rax);
            as_->mov(x86::rcx, static_cast<uint64_t>(magic.magic));
            as_->imul(x86::rcx);

            if (magic.magic < 0) {
                as_->add(x86::rdx, x86::rsi);
            }
            if (magic.shift != 0) {
                as_->sar(x86::rdx, magic.shift);
            }

            as_->mov(x86::rax, x86::rdx);
            as_->shr(x86::rax, 63);
            as_->add(x86::rdx, x86::rax);

            as_->mov(x86::rcx, static_cast<uint64_t>(divisor));
            as_->imul(x86::rcx, x86::rdx);
            as_->mov(x86::rdx, x86::rsi);
            as_->sub(x86::rdx, x86::rcx);
            storeResult(instOff, x86::rdx, inst.type());
            return;
        }
    }

    loadValueToReg(inst.arg(1), x86::rcx);

    auto nonZeroLabel = as_->new_label();
    auto doneLabel = as_->new_label();

    as_->test(x86::rcx, x86::rcx);
    as_->jnz(nonZeroLabel);
    as_->xor_(x86::rdx, x86::rdx);
    as_->jmp(doneLabel);

    as_->bind(nonZeroLabel);
    if (inst.type() == ir::TypeId::INT32) {
        as_->movsxd(x86::rax, x86::eax);
    }
    as_->cqo();
    as_->idiv(x86::rcx);

    as_->bind(doneLabel);
    // Remainder is in RDX.
    storeResult(instOff, x86::rdx, inst.type());
}

void FlyingStartCompiler::translateNeg(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    // NEG is encoded as SUB(0, x): result = arg0 - arg1.
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->sub(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateCheckedArith(ir::Program& program,
                                                const ir::InstructionView& inst, uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);

    uint32_t contBlock = inst.arg(2);
    uint32_t overflowBlock = inst.arg(3);

    // For INT32, operate on 32-bit registers to get correct overflow detection.
    if (inst.type() == ir::TypeId::INT32) {
        switch (inst.opcode()) {
            case ir::Opcode::CHECKED_ADD:
                as_->add(x86::eax, x86::ecx);
                break;
            case ir::Opcode::CHECKED_SUB:
                as_->sub(x86::eax, x86::ecx);
                break;
            case ir::Opcode::CHECKED_MUL:
                as_->imul(x86::eax, x86::ecx);
                break;
            default:
                break;
        }
        // Sign-extend to 64-bit for storage.
        as_->movsxd(x86::rax, x86::eax);
    } else {
        switch (inst.opcode()) {
            case ir::Opcode::CHECKED_ADD:
                as_->add(x86::rax, x86::rcx);
                break;
            case ir::Opcode::CHECKED_SUB:
                as_->sub(x86::rax, x86::rcx);
                break;
            case ir::Opcode::CHECKED_MUL:
                as_->imul(x86::rax, x86::rcx);
                break;
            default:
                break;
        }
    }

    // Store result before branching.
    storeResult(instOff, x86::rax, inst.type());

    // On overflow, jump to overflow block.
    // IMPORTANT: jo must come immediately after the arithmetic, before resolvePhis
    // clobbers the flags. So capture the overflow flag first.
    as_->seto(x86::dl);

    resolvePhis(program, overflowBlock);
    resolvePhis(program, contBlock);

    as_->test(x86::dl, x86::dl);
    as_->jnz(blockLabels_[overflowBlock]);
    as_->jmp(blockLabels_[contBlock]);
}

void FlyingStartCompiler::translateBitwiseAnd(ir::Program& /*program*/,
                                              const ir::InstructionView& inst, uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->and_(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateBitwiseOr(ir::Program& /*program*/,
                                             const ir::InstructionView& inst, uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->or_(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateBitwiseXor(ir::Program& /*program*/,
                                              const ir::InstructionView& inst, uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->xor_(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateShl(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->shl(x86::rax, x86::cl);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateAShr(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->sar(x86::rax, x86::cl);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateLShr(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->shr(x86::rax, x86::cl);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateRotl(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->rol(x86::rax, x86::cl);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateRotr(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->ror(x86::rax, x86::cl);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateCmp(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    // Check if we can defer this comparison for fusion with condbr.
    if (canFuseComparisonBranch(*currentProgram_, lsa_, instOff)) {
        // The pre-allocated stack slot was already returned to the free pool
        // during the pre-allocation pass (at the comparison's last-use point).
        // Just overwrite the location entry with DEFERRED.
        ValueLocation loc;
        loc.kind = LocationKind::DEFERRED;
        loc.deferredOp = instOff;
        loc.type = ir::TypeId::BOOL;
        loc.conditionCode = comparisonConditionCode(inst.opcode());
        regAlloc_.setLocation(instOff, loc);
        return;
    }

    // Non-fused: emit cmp + setcc.
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);
    as_->cmp(x86::rax, x86::rcx);

    auto cc = static_cast<x86::CondCode>(comparisonConditionCode(inst.opcode()));
    as_->set(cc, x86::dl);
    as_->movzx(x86::rdx, x86::dl);
    storeResult(instOff, x86::rdx, ir::TypeId::BOOL);
}

void FlyingStartCompiler::translateLNot(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    as_->test(x86::rax, x86::rax);
    as_->sete(x86::dl);
    as_->movzx(x86::rdx, x86::dl);
    storeResult(instOff, x86::rdx, ir::TypeId::BOOL);
}

void FlyingStartCompiler::translateLAnd(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);

    as_->test(x86::rax, x86::rax);
    as_->setne(x86::dl);
    as_->test(x86::rcx, x86::rcx);
    as_->setne(x86::al);
    as_->and_(x86::dl, x86::al);
    as_->movzx(x86::rdx, x86::dl);
    storeResult(instOff, x86::rdx, ir::TypeId::BOOL);
}

void FlyingStartCompiler::translateLOr(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    loadValueToReg(inst.arg(1), x86::rcx);

    as_->or_(x86::rax, x86::rcx);
    as_->test(x86::rax, x86::rax);
    as_->setne(x86::dl);
    as_->movzx(x86::rdx, x86::dl);
    storeResult(instOff, x86::rdx, ir::TypeId::BOOL);
}

void FlyingStartCompiler::translateSelect(ir::Program& /*program*/, const ir::InstructionView& inst,
                                          uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);  // condition
    loadValueToReg(inst.arg(1), x86::rcx);  // true value
    loadValueToReg(inst.arg(2), x86::rdx);  // false value

    as_->test(x86::rax, x86::rax);
    as_->cmovnz(x86::rdx, x86::rcx);  // if cond != 0, rdx = rcx (true val)
    storeResult(instOff, x86::rdx, inst.type());
}

void FlyingStartCompiler::translateSExt(ir::Program& program, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);

    ir::InstructionView srcInst(program.instBuffer().data(), inst.arg(0));
    auto srcType = srcInst.type();

    if (srcType == ir::TypeId::INT8 || srcType == ir::TypeId::BOOL) {
        as_->movsx(x86::rax, x86::al);
    } else if (srcType == ir::TypeId::INT32) {
        as_->movsxd(x86::rax, x86::eax);
    }
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateZExt(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateTrunc(ir::Program& /*program*/, const ir::InstructionView& inst,
                                         uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);

    if (inst.type() == ir::TypeId::INT32) {
        as_->mov(x86::eax, x86::eax);  // Zero-extends to 64-bit.
    } else if (inst.type() == ir::TypeId::INT8 || inst.type() == ir::TypeId::BOOL) {
        as_->movzx(x86::rax, x86::al);
    }
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateBitcast(ir::Program& /*program*/,
                                           const ir::InstructionView& inst, uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    storeResult(instOff, x86::rax, inst.type());
}

void FlyingStartCompiler::translateLoad(ir::Program& /*program*/, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    const auto* ptrLoc = regAlloc_.getLocation(inst.arg(0));
    bool deferredGep = ptrLoc != nullptr && ptrLoc->kind == LocationKind::DEFERRED &&
                       ptrLoc->gepBase != ir::INVALID_OP;

    if (deferredGep) {
        loadValueToReg(ptrLoc->gepBase, x86::rax);
        loadValueToReg(ptrLoc->gepIndex, x86::rcx);
    } else {
        loadValueToReg(inst.arg(0), x86::rax);  // ptr
    }

    auto type = inst.type();
    switch (type) {
        case ir::TypeId::BOOL:
        case ir::TypeId::INT8:
            if (deferredGep) {
                as_->movsx(x86::rdx,
                           x86::byte_ptr(x86::rax, x86::rcx, gepScaleShift(ptrLoc->gepElemType)));
            } else {
                as_->movsx(x86::rcx, x86::byte_ptr(x86::rax));
            }
            break;
        case ir::TypeId::INT32:
            if (deferredGep) {
                as_->movsxd(x86::rdx,
                            x86::dword_ptr(x86::rax, x86::rcx, gepScaleShift(ptrLoc->gepElemType)));
            } else {
                as_->movsxd(x86::rcx, x86::dword_ptr(x86::rax));
            }
            break;
        case ir::TypeId::INT64:
        case ir::TypeId::UINT64:
        case ir::TypeId::PTR:
            if (deferredGep) {
                as_->mov(x86::rdx,
                         x86::qword_ptr(x86::rax, x86::rcx, gepScaleShift(ptrLoc->gepElemType)));
            } else {
                as_->mov(x86::rcx, x86::qword_ptr(x86::rax));
            }
            break;
        case ir::TypeId::VOID:
            break;
    }
    storeResult(instOff, deferredGep ? x86::rdx : x86::rcx, type);
}

void FlyingStartCompiler::translateStore(ir::Program& program, const ir::InstructionView& inst,
                                         uint32_t /*instOff*/) {
    loadValueToReg(inst.arg(0), x86::rax);  // value

    const auto* ptrLoc = regAlloc_.getLocation(inst.arg(1));
    bool deferredGep = ptrLoc != nullptr && ptrLoc->kind == LocationKind::DEFERRED &&
                       ptrLoc->gepBase != ir::INVALID_OP;

    if (deferredGep) {
        loadValueToReg(ptrLoc->gepBase, x86::rcx);
        loadValueToReg(ptrLoc->gepIndex, x86::rdx);
    } else {
        loadValueToReg(inst.arg(1), x86::rcx);  // ptr
    }

    ir::InstructionView srcInst(program.instBuffer().data(), inst.arg(0));
    auto srcType = srcInst.type();

    switch (srcType) {
        case ir::TypeId::BOOL:
        case ir::TypeId::INT8:
            if (deferredGep) {
                as_->mov(x86::byte_ptr(x86::rcx, x86::rdx, gepScaleShift(ptrLoc->gepElemType)),
                         x86::al);
            } else {
                as_->mov(x86::byte_ptr(x86::rcx), x86::al);
            }
            break;
        case ir::TypeId::INT32:
            if (deferredGep) {
                as_->mov(x86::dword_ptr(x86::rcx, x86::rdx, gepScaleShift(ptrLoc->gepElemType)),
                         x86::eax);
            } else {
                as_->mov(x86::dword_ptr(x86::rcx), x86::eax);
            }
            break;
        case ir::TypeId::INT64:
        case ir::TypeId::UINT64:
        case ir::TypeId::PTR:
            if (deferredGep) {
                as_->mov(x86::qword_ptr(x86::rcx, x86::rdx, gepScaleShift(ptrLoc->gepElemType)),
                         x86::rax);
            } else {
                as_->mov(x86::qword_ptr(x86::rcx), x86::rax);
            }
            break;
        case ir::TypeId::VOID:
            break;
    }
}

void FlyingStartCompiler::translateGep(ir::Program& /*program*/, const ir::InstructionView& inst,
                                       uint32_t instOff) {
    if (canDeferGep(*currentProgram_, lsa_, instOff)) {
        ValueLocation loc;
        loc.kind = LocationKind::DEFERRED;
        loc.gepBase = inst.arg(0);
        loc.gepIndex = inst.arg(1);
        loc.gepElemType = inst.type();
        loc.type = ir::TypeId::PTR;
        regAlloc_.setLocation(instOff, loc);
        return;
    }

    loadValueToReg(inst.arg(0), x86::rax);  // base
    loadValueToReg(inst.arg(1), x86::rcx);  // index

    auto elemSize = static_cast<int64_t>(ir::typeSize(inst.type()));

    if (elemSize != 1) {
        as_->imul(x86::rcx, x86::rcx, static_cast<int32_t>(elemSize));
    }
    as_->add(x86::rax, x86::rcx);
    storeResult(instOff, x86::rax, ir::TypeId::PTR);
}

void FlyingStartCompiler::translateBr(ir::Program& program, const ir::InstructionView& inst) {
    uint32_t targetBlockOff = inst.arg(0);
    resolvePhis(program, targetBlockOff);
    as_->jmp(blockLabels_[targetBlockOff]);
}

void FlyingStartCompiler::translateCondBr(ir::Program& program, const ir::InstructionView& inst) {
    ir::IROp condOp = inst.arg(0);
    uint32_t trueBlockOff = inst.arg(1);
    uint32_t falseBlockOff = inst.arg(2);

    const auto* condLoc = regAlloc_.getLocation(condOp);

    // Optimization 4: Comparison-branch fusion.
    if (condLoc != nullptr && condLoc->kind == LocationKind::DEFERRED) {
        ir::InstructionView cmpInst(currentProgram_->instBuffer().data(), condLoc->deferredOp);

        loadValueToReg(cmpInst.arg(0), x86::rax);
        loadValueToReg(cmpInst.arg(1), x86::rcx);
        as_->cmp(x86::rax, x86::rcx);

        auto cc = static_cast<x86::CondCode>(condLoc->conditionCode);

        // Resolve PHIs for both branches before emitting jumps.
        // Note: resolvePhis for the true branch must come before the conditional jump.
        // But resolvePhis might clobber registers and flags!
        // So we use the approach: setcc to a register, then resolve PHIs, then test+jump.
        as_->set(cc, x86::dl);

        resolvePhis(program, trueBlockOff);
        resolvePhis(program, falseBlockOff);

        as_->test(x86::dl, x86::dl);
        as_->jnz(blockLabels_[trueBlockOff]);
        as_->jmp(blockLabels_[falseBlockOff]);
        return;
    }

    // Non-fused: load condition, test, branch.
    loadValueToReg(condOp, x86::rax);

    // Resolve PHIs before branching.
    // Important: PHI resolution can clobber rax, so save the condition.
    as_->mov(x86::rsi, x86::rax);
    resolvePhis(program, trueBlockOff);
    resolvePhis(program, falseBlockOff);

    as_->test(x86::rsi, x86::rsi);
    as_->jnz(blockLabels_[trueBlockOff]);
    as_->jmp(blockLabels_[falseBlockOff]);
}

void FlyingStartCompiler::translateRet(ir::Program& /*program*/, const ir::InstructionView& inst) {
    loadValueToReg(inst.arg(0), x86::rax);
    as_->jmp(epilogueLabel_);
}

void FlyingStartCompiler::translateRetVoid() {
    as_->xor_(x86::rax, x86::rax);
    as_->jmp(epilogueLabel_);
}

void FlyingStartCompiler::translateCall(ir::Program& program, const ir::InstructionView& inst,
                                        uint32_t instOff) {
    auto calleeIdx = static_cast<ir::FunctionId>(inst.arg(0));
    auto paramCount = ir::callParamCount(inst.opcode());

    const auto* hostFunc = program.hostFunction(calleeIdx);
    if (hostFunc != nullptr) {
        // Build args array on the stack (below our frame).
        // Use a fixed area at the bottom of our frame.
        int32_t argsBase = -4096 + 64;  // Leave some space at the bottom.

        for (uint8_t i = 0; i < paramCount; ++i) {
            loadValueToReg(inst.arg(static_cast<uint8_t>(i + 1)), x86::rax);
            int32_t offset = argsBase + static_cast<int32_t>(i) * 8;
            as_->mov(x86::qword_ptr(x86::rbp, offset), x86::rax);
        }

        // Call jitCallHostFunction(func*, args*, count).
        // System V ABI: rdi=func*, rsi=args*, rdx=count.
        as_->mov(x86::rdi, reinterpret_cast<uint64_t>(hostFunc));  // NOLINT
        as_->lea(x86::rsi, x86::qword_ptr(x86::rbp, argsBase));
        as_->mov(x86::rdx, static_cast<uint64_t>(paramCount));  // NOLINT

        as_->mov(x86::rax, reinterpret_cast<uint64_t>(&jitCallHostFunction));  // NOLINT
        as_->call(x86::rax);

        // Result in RAX.
        storeResult(instOff, x86::rax, inst.type());
    }
}

void FlyingStartCompiler::translateIsNull(ir::Program& /*program*/, const ir::InstructionView& inst,
                                          uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);
    as_->test(x86::rax, x86::rax);
    as_->setne(x86::dl);
    as_->movzx(x86::rdx, x86::dl);
    storeResult(instOff, x86::rdx, ir::TypeId::BOOL);
}

void FlyingStartCompiler::translateCrc32(ir::Program& /*program*/, const ir::InstructionView& inst,
                                         uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);  // data
    loadValueToReg(inst.arg(1), x86::rcx);  // seed

    // crc32 r32, r/m64: accumulates CRC of 8-byte data into 32-bit reg.
    as_->mov(x86::edx, x86::ecx);  // seed into edx (32-bit)
    as_->crc32(x86::edx, x86::rax);

    // Result in edx (zero-extended to rdx).
    storeResult(instOff, x86::rdx, ir::TypeId::UINT64);
}

void FlyingStartCompiler::translateBSwap(ir::Program& /*program*/, const ir::InstructionView& inst,
                                         uint32_t instOff) {
    loadValueToReg(inst.arg(0), x86::rax);

    if (inst.type() == ir::TypeId::INT32) {
        as_->bswap(x86::eax);
        // eax is zero-extended to rax automatically.
    } else {
        as_->bswap(x86::rax);
    }
    storeResult(instOff, x86::rax, inst.type());
}

// ── Stack space reuse: free operands at last use ─────────────────────────────

void FlyingStartCompiler::freeLastUseOperands(const ir::InstructionView& inst, uint32_t instOff) {
    // Special case: COND_BR consuming a deferred comparison — free the cmp's operands.
    if (inst.opcode() == ir::Opcode::COND_BR) {
        ir::IROp condOp = inst.arg(0);
        const auto* condLoc = regAlloc_.getLocation(condOp);
        if (condLoc != nullptr && condLoc->kind == LocationKind::DEFERRED) {
            ir::InstructionView cmpInst(currentProgram_->instBuffer().data(), condLoc->deferredOp);
            uint32_t cmpSeq = lsa_.seqNum(condLoc->deferredOp);
            ir::forEachValueArg(cmpInst, [&](ir::IROp op) { freeIfLastUse(op, cmpSeq); });
        }
        freeIfLastUse(condOp, currentSeq_);
        return;
    }

    if (inst.opcode() == ir::Opcode::LOAD || inst.opcode() == ir::Opcode::STORE) {
        ir::IROp ptrOp = inst.opcode() == ir::Opcode::LOAD ? inst.arg(0) : inst.arg(1);
        const auto* ptrLoc = regAlloc_.getLocation(ptrOp);
        if (ptrLoc != nullptr && ptrLoc->kind == LocationKind::DEFERRED &&
            ptrLoc->gepBase != ir::INVALID_OP) {
            uint32_t gepSeq = lsa_.seqNum(ptrOp);
            freeIfLastUse(ptrLoc->gepBase, gepSeq);
            freeIfLastUse(ptrLoc->gepIndex, gepSeq);
            if (inst.opcode() == ir::Opcode::STORE) {
                freeIfLastUse(inst.arg(0), currentSeq_);
            }
            freeIfLastUse(ptrOp, currentSeq_);
            return;
        }
    }

    // Deferred instructions (fused cmp): operands freed later by COND_BR above.
    const auto* loc = regAlloc_.getLocation(instOff);
    if (loc != nullptr && loc->kind == LocationKind::DEFERRED) {
        return;
    }

    // Generic dispatch via forEachValueArg (handles all opcode-specific arg layouts).
    ir::forEachValueArg(inst, [&](ir::IROp op) { freeIfLastUse(op, currentSeq_); });
}

// ── PHI resolution ───────────────────────────────────────────────────────────

void FlyingStartCompiler::resolvePhis(ir::Program& program, uint32_t targetBlockOff) {
    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();

    // Collect PHI moves: (phi_result_offset, incoming_value_offset).
    std::vector<std::pair<ir::IROp, ir::IROp>> moves;

    uint32_t pos = targetBlockOff;
    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        auto opcode = static_cast<ir::Opcode>(instBuf[instOff]);

        if (!ir::isPhi(opcode)) {
            break;
        }

        ir::InstructionView inst(instBuf.data(), instOff);
        auto count = ir::phiIncomingCount(opcode);

        for (uint8_t i = 0; i < count; ++i) {
            uint32_t incomingBlock = inst.arg(static_cast<uint8_t>(i * 2));
            uint32_t incomingValue = inst.arg(static_cast<uint8_t>(i * 2 + 1));
            if (incomingBlock == currentBlockOff_) {
                moves.emplace_back(instOff, incomingValue);
                break;
            }
        }
        ++pos;
    }

    if (moves.empty()) {
        return;
    }

    // First collect all source values into temporaries (to handle parallel assignment).
    // For simplicity, first load all sources into scratch stack area, then write to PHI slots.
    // We use RSI as scratch register (not used by PHI loads/stores which use RAX).

    // Phase 1: Load all incoming values to a temporary area.
    int32_t tempBase = -4096 + 256;  // Temp area in our frame.
    for (size_t i = 0; i < moves.size(); ++i) {
        loadValueToReg(moves[i].second, x86::rax);
        int32_t tempOff = tempBase + static_cast<int32_t>(i) * 8;
        as_->mov(x86::qword_ptr(x86::rbp, tempOff), x86::rax);
    }

    // Phase 2: Write from temp area to PHI result locations.
    for (size_t i = 0; i < moves.size(); ++i) {
        int32_t tempOff = tempBase + static_cast<int32_t>(i) * 8;
        as_->mov(x86::rax, x86::qword_ptr(x86::rbp, tempOff));

        // Use assignResultLocation for robustness (PHIs are pre-allocated,
        // but this is a safety net).
        ir::InstructionView phiInst(instBuf.data(), moves[i].first);
        auto phiLoc = assignResultLocation(moves[i].first, phiInst.type());
        if (phiLoc.kind == LocationKind::STACK) {
            as_->mov(x86::qword_ptr(x86::rbp, phiLoc.stackOffset), x86::rax);
        }
    }
}

}  // namespace umbra::flying_start
