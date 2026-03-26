#include "backend/flying_start/optimizations.h"

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <cstdint>

#include "backend/flying_start/live_span.h"
#include "backend/flying_start/register_alloc.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

// ── Optimization 2: Register allocation heuristic ────────────────────────────

bool shouldAllocateRegister(const LiveSpanAnalysis& lsa, ir::IROp op, uint32_t currentBlockOff,
                            const RegisterAllocator& regAlloc) {
    if (!regAlloc.hasAvailableRegister()) {
        return false;
    }

    const auto* span = lsa.getSpan(op);
    if (span == nullptr) {
        return false;
    }

    uint32_t defBlock = lsa.blockOf(op);
    if (defBlock == currentBlockOff) {
        return true;
    }
    return regAlloc.availableRegisterCount() > 3;
}

// ── Optimization 4: Comparison-branch fusion ──────────────────────────���──────

bool canFuseComparisonBranch(const ir::Program& program, const LiveSpanAnalysis& lsa,
                             ir::IROp cmpOp) {
    ir::InstructionView cmpInst(program.instBuffer().data(), cmpOp);
    auto opcode = cmpInst.opcode();

    // Only comparison opcodes.
    if (opcode < ir::Opcode::CMP_EQ || opcode > ir::Opcode::CMP_GE) {
        return false;
    }

    const auto* span = lsa.getSpan(cmpOp);
    if (span == nullptr) {
        return false;
    }

    // The comparison must be used exactly once, and the user must be a COND_BR.
    // We verify this by scanning the same block for the consumer.
    uint32_t cmpBlock = lsa.blockOf(cmpOp);
    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();

    // Find the cmpOp in its block, then check if the next instruction after it
    // (or the one after that) is a COND_BR that references it.
    uint32_t pos = cmpBlock;
    bool foundCmp = false;
    int useCount = 0;
    bool usedByCondBr = false;

    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        auto instOpcode = static_cast<ir::Opcode>(instBuf[instOff]);
        ++pos;

        if (instOff == cmpOp) {
            foundCmp = true;
        } else if (foundCmp) {
            // Check if this instruction uses the comparison result.
            ir::InstructionView consumer(instBuf.data(), instOff);
            auto numArgs = consumer.numArgs();
            for (uint8_t i = 0; i < numArgs; ++i) {
                if (consumer.arg(i) == cmpOp) {
                    ++useCount;
                    if (instOpcode == ir::Opcode::COND_BR && i == 0) {
                        usedByCondBr = true;
                    }
                }
            }
        }

        if (ir::isTerminator(instOpcode)) {
            break;
        }
    }

    // Only fuse if the comparison is used exactly once, by a COND_BR as its condition.
    return foundCmp && useCount == 1 && usedByCondBr;
}

// ── Optimization 3: Lazy address calculation ─────────────────────���───────────

bool canDeferGep(const ir::Program& program, const LiveSpanAnalysis& lsa, ir::IROp gepOp) {
    ir::InstructionView gepInst(program.instBuffer().data(), gepOp);
    if (gepInst.opcode() != ir::Opcode::GEP) {
        return false;
    }

    const auto* span = lsa.getSpan(gepOp);
    if (span == nullptr) {
        return false;
    }

    if ((span->lastUsePoint - span->defPoint) > 2) {
        return false;
    }

    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();
    uint32_t pos = lsa.blockOf(gepOp);
    bool seenGep = false;

    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        auto opcode = static_cast<ir::Opcode>(instBuf[instOff]);
        ++pos;

        if (instOff == gepOp) {
            seenGep = true;
            continue;
        }
        if (!seenGep) {
            if (ir::isTerminator(opcode)) {
                break;
            }
            continue;
        }

        ir::InstructionView user(program.instBuffer().data(), instOff);
        for (uint8_t i = 0; i < user.numArgs(); ++i) {
            if (user.arg(i) != gepOp) {
                continue;
            }
            if (opcode != ir::Opcode::LOAD && opcode != ir::Opcode::STORE) {
                return false;
            }
        }

        if (ir::isTerminator(opcode)) {
            break;
        }
    }

    return true;
}

// ── Condition code mapping ────────���────────────────────────────���─────────────

uint8_t comparisonConditionCode(ir::Opcode opcode) {
    using namespace asmjit::x86;
    switch (opcode) {
        case ir::Opcode::CMP_EQ:
            return static_cast<uint8_t>(CondCode::kE);
        case ir::Opcode::CMP_NE:
            return static_cast<uint8_t>(CondCode::kNE);
        case ir::Opcode::CMP_LT:
            return static_cast<uint8_t>(CondCode::kL);
        case ir::Opcode::CMP_LE:
            return static_cast<uint8_t>(CondCode::kLE);
        case ir::Opcode::CMP_GT:
            return static_cast<uint8_t>(CondCode::kG);
        case ir::Opcode::CMP_GE:
            return static_cast<uint8_t>(CondCode::kGE);
        default:
            return static_cast<uint8_t>(CondCode::kE);
    }
}

uint8_t inverseConditionCode(uint8_t cc) {
    using namespace asmjit::x86;
    switch (static_cast<CondCode>(cc)) {
        case CondCode::kE:
            return static_cast<uint8_t>(CondCode::kNE);
        case CondCode::kNE:
            return static_cast<uint8_t>(CondCode::kE);
        case CondCode::kL:
            return static_cast<uint8_t>(CondCode::kGE);
        case CondCode::kLE:
            return static_cast<uint8_t>(CondCode::kG);
        case CondCode::kG:
            return static_cast<uint8_t>(CondCode::kLE);
        case CondCode::kGE:
            return static_cast<uint8_t>(CondCode::kL);
        default:
            return cc;
    }
}

}  // namespace umbra::flying_start
