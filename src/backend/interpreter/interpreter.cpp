#include "backend/interpreter/interpreter.h"

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/printer.h"
#include "ir/program.h"

namespace umbra::interpreter {

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace {

/// Sign-extend a value from fromBits to 64 bits.
int64_t signExtend(uint64_t val, int fromBits) {
    int shift = 64 - fromBits;
    return static_cast<int64_t>(static_cast<int64_t>(val << shift) >> shift);
}

/// Software CRC32C implementation (compatible with SSE4.2 crc32 instruction).
uint32_t softCrc32Byte(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        if ((crc & 1U) != 0) {
            crc = (crc >> 1) ^ 0x82F63B78U;  // CRC32C polynomial
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

uint64_t softCrc32(uint64_t seed, uint64_t data) {
    auto crc = static_cast<uint32_t>(seed);
    for (int i = 0; i < 8; ++i) {
        crc = softCrc32Byte(crc, static_cast<uint8_t>(data & 0xFF));
        data >>= 8;
    }
    return crc;
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────────

uint64_t Interpreter::execute(ir::Program& program, ir::FunctionId func,
                              std::span<const uint64_t> args) {
    const auto& entry = program.functionEntry(func);
    assert(entry.entryBlockOffset != ir::INVALID_BLOCK && "Cannot execute host function directly");

    // Allocate value slots indexed by instruction buffer byte offset.
    slots_.assign(program.instBuffer().size(), 0);

    // Initialize function arguments.
    initFunArgs(program, entry.entryBlockOffset, args);

    // Execute from the entry block.
    return executeFrom(program, entry.entryBlockOffset);
}

// ── Execution loop ───────────────────────────────────────────────────────────

uint64_t Interpreter::executeFrom(ir::Program& program, uint32_t blockOff) {
    uint32_t prevBlockOff = INVALID_PREV;

    while (true) {
        // Step 1: resolve PHIs, get position of first non-PHI instruction.
        uint32_t pos = resolvePhis(program, blockOff, prevBlockOff);

        // Step 2: execute non-PHI instructions until terminator.
        const auto& blockBuf = program.blockBuffer();
        const auto& instBuf = program.instBuffer();

        while (true) {
            assert(pos < static_cast<uint32_t>(blockBuf.size()));
            uint32_t instOff = blockBuf[pos];
            ++pos;

            ir::InstructionView inst(instBuf.data(), instOff);
            assert(!ir::isPhi(inst.opcode()) && "PHI should have been handled by resolvePhis");

            if (traceEnabled_) {
                traceInst(program, inst, instOff);
            }

            ExecuteResult result = executeInst(program, inst, instOff);

            if (result.type == ExecuteResult::CONTINUE) {
                continue;
            }
            if (result.type == ExecuteResult::BRANCH) {
                prevBlockOff = blockOff;
                blockOff = result.targetBlockOff;
                break;
            }
            if (result.type == ExecuteResult::RETURN) {
                return result.returnValue;
            }
        }
    }
}

// ── Argument initialization ──────────────────────────────────────────────────

void Interpreter::initFunArgs(ir::Program& program, uint32_t entryBlockOff,
                              std::span<const uint64_t> args) {
    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();

    uint32_t pos = entryBlockOff;
    uint32_t argIdx = 0;

    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        auto opcode = static_cast<ir::Opcode>(instBuf[instOff]);

        if (opcode != ir::Opcode::FUNARG) {
            break;
        }

        if (argIdx < args.size()) {
            slots_[instOff] = args[argIdx];
        }
        ++argIdx;
        ++pos;
    }
}

// ── PHI resolution ───────────────────────────────────────────────────────────

uint32_t Interpreter::resolvePhis(ir::Program& program, uint32_t blockOff, uint32_t prevBlockOff) {
    const auto& blockBuf = program.blockBuffer();
    const auto& instBuf = program.instBuffer();
    uint32_t pos = blockOff;

    // If no predecessor (first entry to entry block), skip all PHIs.
    if (prevBlockOff == INVALID_PREV) {
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            auto opcode = static_cast<ir::Opcode>(instBuf[instOff]);
            if (!ir::isPhi(opcode)) {
                return pos;
            }
            ++pos;
        }
        return pos;
    }

    // Phase 1: collect all PHI results (dst_slot, value) without writing.
    std::vector<std::pair<uint32_t, uint64_t>> phiResults;

    while (pos < static_cast<uint32_t>(blockBuf.size())) {
        uint32_t instOff = blockBuf[pos];
        ir::InstructionView inst(instBuf.data(), instOff);

        if (!ir::isPhi(inst.opcode())) {
            break;
        }

        auto count = ir::phiIncomingCount(inst.opcode());
        for (uint8_t i = 0; i < count; ++i) {
            uint32_t incomingBlock = inst.arg(static_cast<uint8_t>(i * 2));
            uint32_t incomingValue = inst.arg(static_cast<uint8_t>(i * 2 + 1));
            if (incomingBlock == prevBlockOff) {
                phiResults.emplace_back(instOff, slots_[incomingValue]);
                break;
            }
        }
        ++pos;
    }

    // Phase 2: write all PHI values (parallel assignment).
    for (const auto& [dst, val] : phiResults) {
        slots_[dst] = val;
    }

    return pos;
}

// ── Instruction execution ────────────────────────────────────────────────────

Interpreter::ExecuteResult Interpreter::executeInst(ir::Program& program,
                                                    const ir::InstructionView& inst,
                                                    uint32_t instOff) {
    using ir::Opcode;
    using ir::TypeId;

    auto opcode = inst.opcode();
    auto type = inst.type();

    switch (opcode) {
        // ── Constants and arguments ──────────────────────────────────────
        case Opcode::CONST: {
            slots_[instOff] = inst.constPayload();
            return ExecuteResult::makeContinue();
        }

        case Opcode::FUNARG: {
            // Already initialized in initFunArgs — no-op.
            return ExecuteResult::makeContinue();
        }

        // ── Arithmetic ──────────────────────────────────────────────────
        case Opcode::ADD: {
            if (type == TypeId::INT32) {
                auto result = static_cast<int32_t>(slots_[inst.arg(0)]) +
                              static_cast<int32_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else if (type == TypeId::UINT64) {
                slots_[instOff] = slots_[inst.arg(0)] + slots_[inst.arg(1)];
            } else {
                auto result = static_cast<int64_t>(slots_[inst.arg(0)]) +
                              static_cast<int64_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::SUB: {
            if (type == TypeId::INT32) {
                auto result = static_cast<int32_t>(slots_[inst.arg(0)]) -
                              static_cast<int32_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else if (type == TypeId::UINT64) {
                slots_[instOff] = slots_[inst.arg(0)] - slots_[inst.arg(1)];
            } else {
                auto result = static_cast<int64_t>(slots_[inst.arg(0)]) -
                              static_cast<int64_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::MUL: {
            if (type == TypeId::INT32) {
                auto result = static_cast<int32_t>(slots_[inst.arg(0)]) *
                              static_cast<int32_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else if (type == TypeId::UINT64) {
                // Unsigned multiplication wraps without UB.
                slots_[instOff] = slots_[inst.arg(0)] * slots_[inst.arg(1)];
            } else {
                auto result = static_cast<int64_t>(slots_[inst.arg(0)]) *
                              static_cast<int64_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::DIV: {
            if (type == TypeId::INT32) {
                auto divisor = static_cast<int32_t>(slots_[inst.arg(1)]);
                if (divisor == 0) {
                    slots_[instOff] = 0;
                } else {
                    auto result = static_cast<int32_t>(slots_[inst.arg(0)]) / divisor;
                    slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
                }
            } else {
                auto divisor = static_cast<int64_t>(slots_[inst.arg(1)]);
                if (divisor == 0) {
                    slots_[instOff] = 0;
                } else {
                    auto result = static_cast<int64_t>(slots_[inst.arg(0)]) / divisor;
                    slots_[instOff] = static_cast<uint64_t>(result);
                }
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::UDIV: {
            uint64_t divisor = slots_[inst.arg(1)];
            if (divisor == 0) {
                slots_[instOff] = 0;
            } else {
                slots_[instOff] = slots_[inst.arg(0)] / divisor;
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::MOD: {
            if (type == TypeId::INT32) {
                auto divisor = static_cast<int32_t>(slots_[inst.arg(1)]);
                if (divisor == 0) {
                    slots_[instOff] = 0;
                } else {
                    auto result = static_cast<int32_t>(slots_[inst.arg(0)]) % divisor;
                    slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
                }
            } else {
                auto divisor = static_cast<int64_t>(slots_[inst.arg(1)]);
                if (divisor == 0) {
                    slots_[instOff] = 0;
                } else {
                    auto result = static_cast<int64_t>(slots_[inst.arg(0)]) % divisor;
                    slots_[instOff] = static_cast<uint64_t>(result);
                }
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::NEG: {
            // Encoded as binary with zero first arg: NEG(0, x) = 0 - x
            if (type == TypeId::INT32) {
                auto result = static_cast<int32_t>(slots_[inst.arg(0)]) -
                              static_cast<int32_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else {
                auto result = static_cast<int64_t>(slots_[inst.arg(0)]) -
                              static_cast<int64_t>(slots_[inst.arg(1)]);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return ExecuteResult::makeContinue();
        }

        // ── Checked arithmetic ──────────────────────────────────────────
        case Opcode::CHECKED_ADD: {
            uint32_t contBlock = inst.arg(2);
            uint32_t overflowBlock = inst.arg(3);
            bool overflow = false;
            if (type == TypeId::INT32) {
                int32_t result = 0;
                overflow =
                    __builtin_add_overflow(static_cast<int32_t>(slots_[inst.arg(0)]),
                                           static_cast<int32_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else {
                int64_t result = 0;
                overflow =
                    __builtin_add_overflow(static_cast<int64_t>(slots_[inst.arg(0)]),
                                           static_cast<int64_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return overflow ? ExecuteResult::makeBranch(overflowBlock)
                            : ExecuteResult::makeBranch(contBlock);
        }

        case Opcode::CHECKED_SUB: {
            uint32_t contBlock = inst.arg(2);
            uint32_t overflowBlock = inst.arg(3);
            bool overflow = false;
            if (type == TypeId::INT32) {
                int32_t result = 0;
                overflow =
                    __builtin_sub_overflow(static_cast<int32_t>(slots_[inst.arg(0)]),
                                           static_cast<int32_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else {
                int64_t result = 0;
                overflow =
                    __builtin_sub_overflow(static_cast<int64_t>(slots_[inst.arg(0)]),
                                           static_cast<int64_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return overflow ? ExecuteResult::makeBranch(overflowBlock)
                            : ExecuteResult::makeBranch(contBlock);
        }

        case Opcode::CHECKED_MUL: {
            uint32_t contBlock = inst.arg(2);
            uint32_t overflowBlock = inst.arg(3);
            bool overflow = false;
            if (type == TypeId::INT32) {
                int32_t result = 0;
                overflow =
                    __builtin_mul_overflow(static_cast<int32_t>(slots_[inst.arg(0)]),
                                           static_cast<int32_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else {
                int64_t result = 0;
                overflow =
                    __builtin_mul_overflow(static_cast<int64_t>(slots_[inst.arg(0)]),
                                           static_cast<int64_t>(slots_[inst.arg(1)]), &result);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return overflow ? ExecuteResult::makeBranch(overflowBlock)
                            : ExecuteResult::makeBranch(contBlock);
        }

        // ── Bitwise ─────────────────────────────────────────────────────
        case Opcode::AND: {
            slots_[instOff] = slots_[inst.arg(0)] & slots_[inst.arg(1)];
            return ExecuteResult::makeContinue();
        }

        case Opcode::OR: {
            slots_[instOff] = slots_[inst.arg(0)] | slots_[inst.arg(1)];
            return ExecuteResult::makeContinue();
        }

        case Opcode::XOR: {
            slots_[instOff] = slots_[inst.arg(0)] ^ slots_[inst.arg(1)];
            return ExecuteResult::makeContinue();
        }

        case Opcode::SHL: {
            auto shift = static_cast<int>(slots_[inst.arg(1)]);
            if (type == TypeId::INT32) {
                slots_[instOff] = static_cast<uint64_t>(static_cast<uint32_t>(
                    static_cast<uint32_t>(slots_[inst.arg(0)]) << (shift & 31)));
            } else {
                slots_[instOff] = slots_[inst.arg(0)] << (shift & 63);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::ASHR: {
            auto shift = static_cast<int>(slots_[inst.arg(1)]);
            if (type == TypeId::INT32) {
                auto result = static_cast<int32_t>(slots_[inst.arg(0)]) >> (shift & 31);
                slots_[instOff] = static_cast<uint64_t>(static_cast<int64_t>(result));
            } else {
                auto result = static_cast<int64_t>(slots_[inst.arg(0)]) >> (shift & 63);
                slots_[instOff] = static_cast<uint64_t>(result);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::LSHR: {
            auto shift = static_cast<int>(slots_[inst.arg(1)]);
            if (type == TypeId::INT32) {
                slots_[instOff] = static_cast<uint64_t>(
                    static_cast<uint32_t>(slots_[inst.arg(0)]) >> (shift & 31));
            } else {
                slots_[instOff] = slots_[inst.arg(0)] >> (shift & 63);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::ROTL: {
            auto val = slots_[inst.arg(0)];
            auto rot = static_cast<int>(slots_[inst.arg(1)]);
            if (type == TypeId::INT32) {
                auto v32 = static_cast<uint32_t>(val);
                slots_[instOff] = static_cast<uint64_t>(std::rotl(v32, rot));
            } else {
                slots_[instOff] = std::rotl(val, rot);
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::ROTR: {
            auto val = slots_[inst.arg(0)];
            auto rot = static_cast<int>(slots_[inst.arg(1)]);
            if (type == TypeId::INT32) {
                auto v32 = static_cast<uint32_t>(val);
                slots_[instOff] = static_cast<uint64_t>(std::rotr(v32, rot));
            } else {
                slots_[instOff] = std::rotr(val, rot);
            }
            return ExecuteResult::makeContinue();
        }

        // ── Comparison ──────────────────────────────────────────────────
        case Opcode::CMP_EQ: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) ==
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CMP_NE: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) !=
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CMP_LT: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) <
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CMP_LE: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) <=
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CMP_GT: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) >
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CMP_GE: {
            slots_[instOff] = (static_cast<int64_t>(slots_[inst.arg(0)]) >=
                               static_cast<int64_t>(slots_[inst.arg(1)]))
                                  ? 1U
                                  : 0U;
            return ExecuteResult::makeContinue();
        }

        // ── Logical ─────────────────────────────────────────────────────
        case Opcode::LNOT: {
            slots_[instOff] = (slots_[inst.arg(0)] != 0) ? 0U : 1U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::LAND: {
            slots_[instOff] = ((slots_[inst.arg(0)] != 0) && (slots_[inst.arg(1)] != 0)) ? 1U : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::LOR: {
            slots_[instOff] = ((slots_[inst.arg(0)] != 0) || (slots_[inst.arg(1)] != 0)) ? 1U : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::SELECT: {
            bool cond = (slots_[inst.arg(0)] != 0);
            slots_[instOff] = cond ? slots_[inst.arg(1)] : slots_[inst.arg(2)];
            return ExecuteResult::makeContinue();
        }

        // ── Cast ────────────────────────────────────────────────────────
        case Opcode::SEXT: {
            uint64_t val = slots_[inst.arg(0)];
            // Determine source bits from the operand's instruction type.
            ir::InstructionView srcInst(program.instBuffer().data(), inst.arg(0));
            auto srcType = srcInst.type();
            int srcBits = ir::typeSize(srcType) * 8;
            slots_[instOff] = static_cast<uint64_t>(signExtend(val, srcBits));
            return ExecuteResult::makeContinue();
        }

        case Opcode::ZEXT: {
            // Zero extension: just copy bits (upper bits already zero in uint64).
            slots_[instOff] = slots_[inst.arg(0)];
            return ExecuteResult::makeContinue();
        }

        case Opcode::TRUNC: {
            uint64_t val = slots_[inst.arg(0)];
            int dstBits = ir::typeSize(type) * 8;
            if (dstBits < 64) {
                uint64_t mask = (1ULL << dstBits) - 1;
                slots_[instOff] = val & mask;
            } else {
                slots_[instOff] = val;
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::BITCAST:
        case Opcode::PTR2INT:
        case Opcode::INT2PTR: {
            // All three are identity operations at the bit level.
            slots_[instOff] = slots_[inst.arg(0)];
            return ExecuteResult::makeContinue();
        }

        // ── Memory ──────────────────────────────────────────────────────
        case Opcode::LOAD: {
            auto* addr = reinterpret_cast<const void*>(slots_[inst.arg(0)]);  // NOLINT
            uint64_t val = 0;
            switch (type) {
                case TypeId::BOOL:
                case TypeId::INT8: {
                    int8_t tmp = 0;
                    std::memcpy(&tmp, addr, sizeof(int8_t));
                    val = static_cast<uint64_t>(static_cast<int64_t>(tmp));
                    break;
                }
                case TypeId::INT32: {
                    int32_t tmp = 0;
                    std::memcpy(&tmp, addr, sizeof(int32_t));
                    val = static_cast<uint64_t>(static_cast<int64_t>(tmp));
                    break;
                }
                case TypeId::INT64:
                case TypeId::UINT64: {
                    std::memcpy(&val, addr, sizeof(uint64_t));
                    break;
                }
                case TypeId::PTR: {
                    void* ptrVal = nullptr;
                    std::memcpy(&ptrVal, addr, sizeof(void*));
                    val = reinterpret_cast<uint64_t>(ptrVal);  // NOLINT
                    break;
                }
                case TypeId::VOID:
                    break;
            }
            slots_[instOff] = val;
            return ExecuteResult::makeContinue();
        }

        case Opcode::STORE: {
            uint64_t val = slots_[inst.arg(0)];
            auto* addr = reinterpret_cast<void*>(slots_[inst.arg(1)]);  // NOLINT

            // Determine store size from the source operand's type.
            ir::InstructionView srcInst(program.instBuffer().data(), inst.arg(0));
            auto srcType = srcInst.type();
            switch (srcType) {
                case TypeId::BOOL:
                case TypeId::INT8: {
                    auto tmp = static_cast<int8_t>(val);
                    std::memcpy(addr, &tmp, sizeof(int8_t));
                    break;
                }
                case TypeId::INT32: {
                    auto tmp = static_cast<int32_t>(val);
                    std::memcpy(addr, &tmp, sizeof(int32_t));
                    break;
                }
                case TypeId::INT64:
                case TypeId::UINT64: {
                    std::memcpy(addr, &val, sizeof(uint64_t));
                    break;
                }
                case TypeId::PTR: {
                    auto ptrVal = reinterpret_cast<void*>(val);  // NOLINT
                    std::memcpy(addr, &ptrVal, sizeof(void*));
                    break;
                }
                case TypeId::VOID:
                    break;
            }
            return ExecuteResult::makeContinue();
        }

        case Opcode::GEP: {
            auto* base = reinterpret_cast<char*>(slots_[inst.arg(0)]);  // NOLINT
            auto index = static_cast<int64_t>(slots_[inst.arg(1)]);
            auto elemSize = static_cast<int64_t>(ir::typeSize(type));
            slots_[instOff] = reinterpret_cast<uint64_t>(base + index * elemSize);  // NOLINT
            return ExecuteResult::makeContinue();
        }

        // ── Control flow ────────────────────────────────────────────────
        case Opcode::BR: {
            return ExecuteResult::makeBranch(inst.arg(0));
        }

        case Opcode::COND_BR: {
            bool cond = (slots_[inst.arg(0)] != 0);
            return cond ? ExecuteResult::makeBranch(inst.arg(1))
                        : ExecuteResult::makeBranch(inst.arg(2));
        }

        case Opcode::RET: {
            return ExecuteResult::makeReturn(slots_[inst.arg(0)]);
        }

        case Opcode::RET_VOID: {
            return ExecuteResult::makeReturn(0);
        }

        case Opcode::SWITCH: {
            // Not implemented in MVP.
            assert(false && "SWITCH not implemented");
            return ExecuteResult::makeReturn(0);
        }

        // ── Calls ───────────────────────────────────────────────────────
        case Opcode::CALL0:
        case Opcode::CALL1:
        case Opcode::CALL2:
        case Opcode::CALL3:
        case Opcode::CALL4:
        case Opcode::CALL5:
        case Opcode::CALL6: {
            auto calleeIdx = static_cast<ir::FunctionId>(inst.arg(0));
            auto paramCount = ir::callParamCount(opcode);

            // Collect arguments.
            std::vector<uint64_t> callArgs;
            callArgs.reserve(paramCount);
            for (uint8_t i = 0; i < paramCount; ++i) {
                callArgs.push_back(slots_[inst.arg(static_cast<uint8_t>(i + 1))]);
            }

            // Check if it's a host function.
            const auto* hostFunc = program.hostFunction(calleeIdx);
            if (hostFunc != nullptr) {
                slots_[instOff] =
                    (*hostFunc)(callArgs.data(), static_cast<uint8_t>(callArgs.size()));
            } else {
                // Recursive interpretation of an IR function.
                // Save and restore slots since the callee uses the same slot array.
                auto savedSlots = std::move(slots_);
                uint64_t result = execute(program, calleeIdx, std::span<const uint64_t>(callArgs));
                slots_ = std::move(savedSlots);
                slots_[instOff] = result;
            }
            return ExecuteResult::makeContinue();
        }

        // ── PHI — should never be reached here ─────────────────────────
        case Opcode::PHI2:
        case Opcode::PHI3:
        case Opcode::PHI4:
        case Opcode::PHI5:
        case Opcode::PHI6: {
            assert(false && "PHI should be handled by resolvePhis, not executeInst");
            return ExecuteResult::makeContinue();
        }

        // ── DB-specific ─────────────────────────────────────────────────
        case Opcode::IS_NULL: {
            slots_[instOff] = (slots_[inst.arg(0)] != 0) ? 1U : 0U;
            return ExecuteResult::makeContinue();
        }

        case Opcode::CRC32: {
            uint64_t data = slots_[inst.arg(0)];
            uint64_t seed = slots_[inst.arg(1)];
            slots_[instOff] = softCrc32(seed, data);
            return ExecuteResult::makeContinue();
        }

        case Opcode::BSWAP: {
            uint64_t val = slots_[inst.arg(0)];
            if (type == TypeId::INT32) {
                auto v32 = static_cast<uint32_t>(val);
                v32 = __builtin_bswap32(v32);
                slots_[instOff] = static_cast<uint64_t>(v32);
            } else {
                slots_[instOff] = __builtin_bswap64(val);
            }
            return ExecuteResult::makeContinue();
        }

        // ── NOP ─────────────────────────────────────────────────────────
        case Opcode::NOP: {
            return ExecuteResult::makeContinue();
        }

        case Opcode::NUM_OPCODES:
        default: {
            assert(false && "Unknown opcode");
            return ExecuteResult::makeContinue();
        }
    }
}

// ── Host function dispatch ───────────────────────────────────────────────────

uint64_t Interpreter::callHostFunction(const ir::HostFunction& func,
                                       std::span<const uint64_t> args) {
    return func(args.data(), static_cast<uint8_t>(args.size()));
}

// ── Trace ────────────────────────────────────────────────────────────────────

void Interpreter::traceInst(ir::Program& program, const ir::InstructionView& inst,
                            uint32_t instOff) {
    auto& out = (traceOut_ != nullptr) ? *traceOut_ : std::cerr;
    out << "  [%" << instOff << "] " << ir::IRPrinter::printInstruction(program, instOff);

    // Print result value for non-void, non-terminator instructions.
    if (inst.type() != ir::TypeId::VOID && !ir::isTerminator(inst.opcode()) &&
        inst.opcode() != ir::Opcode::CONST && inst.opcode() != ir::Opcode::FUNARG) {
        // Result is already computed only for instructions before this point.
        // For trace, we print after execution in the caller.
    }
    out << "\n";
}

}  // namespace umbra::interpreter
