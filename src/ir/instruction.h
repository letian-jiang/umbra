#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "ir/opcode.h"

namespace umbra::ir {

/// IROp — a lightweight handle (4-byte offset) representing an SSA value.
/// The offset points into the instruction buffer.
using IROp = uint32_t;

/// Sentinel value for invalid/uninitialized IROp.
constexpr IROp INVALID_OP = UINT32_MAX;

/// InstructionView — zero-copy view into the instruction buffer.
/// Provides typed access to an instruction's fields.
class InstructionView {
   public:
    InstructionView(const uint8_t* buffer, uint32_t offset)
        : buffer_(buffer + offset), offset_(offset) {}

    /// The opcode.
    [[nodiscard]] Opcode opcode() const { return static_cast<Opcode>(buffer_[0]); }

    /// The result type.
    [[nodiscard]] TypeId type() const { return static_cast<TypeId>(buffer_[1]); }

    /// Number of args (from static opcode table).
    [[nodiscard]] uint8_t numArgs() const { return opcodeArgCount(opcode()); }

    /// The i-th operand (an IROp offset).
    [[nodiscard]] IROp arg(uint8_t i) const {
        uint32_t val = 0;
        std::memcpy(&val, buffer_ + 2 + static_cast<size_t>(4) * i, sizeof(uint32_t));
        return val;
    }

    /// This instruction's own offset (its IROp handle).
    [[nodiscard]] uint32_t offset() const { return offset_; }

    /// Total byte size of this instruction in the buffer.
    [[nodiscard]] uint32_t byteSize() const {
        uint32_t size = 2 + 4 * static_cast<uint32_t>(numArgs());
        if (opcode() == Opcode::CONST) {
            size += constPayloadSize(type());
        }
        return size;
    }

    /// Read the constant payload as a uint64_t (for CONST instructions).
    /// Sign-extends INT8 and INT32 values to match LOAD semantics.
    [[nodiscard]] uint64_t constPayload() const {
        uint64_t val = 0;
        uint32_t payloadOff = 2;  // after opcode + type, no args for CONST
        auto psize = constPayloadSize(type());
        std::memcpy(&val, buffer_ + payloadOff, psize);
        // Sign-extend signed integer types so constants and loaded values
        // have the same representation in 64-bit slots.
        auto tid = type();
        if (tid == TypeId::INT8) {
            val = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(val)));
        } else if (tid == TypeId::INT32) {
            val = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(val)));
        }
        return val;
    }

    /// Payload size for a constant instruction based on type.
    static uint32_t constPayloadSize(TypeId tid) {
        switch (tid) {
            case TypeId::VOID:
                return 0;
            case TypeId::BOOL:
            case TypeId::INT8:
                return 1;
            case TypeId::INT32:
                return 4;
            case TypeId::INT64:
            case TypeId::UINT64:
            case TypeId::PTR:
                return 8;
        }
        return 0;
    }

   private:
    const uint8_t* buffer_;
    uint32_t offset_;
};

/// Encode an instruction into the instruction buffer.
/// Returns the offset of the newly appended instruction.
inline IROp encodeInstruction(std::vector<uint8_t>& buffer, Opcode opcode, TypeId type,
                              const IROp* args, uint8_t argCount) {
    auto offset = static_cast<uint32_t>(buffer.size());
    buffer.push_back(static_cast<uint8_t>(opcode));
    buffer.push_back(static_cast<uint8_t>(type));
    for (uint8_t i = 0; i < argCount; ++i) {
        uint32_t val = args[i];
        const auto* bytes = reinterpret_cast<const uint8_t*>(&val);
        buffer.insert(buffer.end(), bytes, bytes + 4);
    }
    return offset;
}

/// Encode a constant instruction with its payload.
inline IROp encodeConst(std::vector<uint8_t>& buffer, TypeId type, uint64_t value) {
    auto offset = static_cast<uint32_t>(buffer.size());
    buffer.push_back(static_cast<uint8_t>(Opcode::CONST));
    buffer.push_back(static_cast<uint8_t>(type));
    auto payloadSize = InstructionView::constPayloadSize(type);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), bytes, bytes + payloadSize);
    return offset;
}

/// Patch a 4-byte value at a specific position in the instruction buffer.
inline void patchArg(std::vector<uint8_t>& buffer, uint32_t instOffset, uint8_t argIndex,
                     uint32_t newValue) {
    uint32_t pos = instOffset + 2 + 4 * static_cast<uint32_t>(argIndex);
    std::memcpy(buffer.data() + pos, &newValue, sizeof(uint32_t));
}

/// Iterate over value operands of an instruction, skipping non-value args
/// (block offsets in BR/COND_BR/CHECKED_*, callee ID in CALL*, param index in FUNARG).
/// For PHIs, iterates over incoming values only (not block args).
template <typename Fn>
void forEachValueArg(const InstructionView& inst, Fn&& fn) {
    auto opcode = inst.opcode();
    if (isPhi(opcode)) {
        auto count = phiIncomingCount(opcode);
        for (uint8_t i = 0; i < count; ++i) {
            fn(inst.arg(static_cast<uint8_t>(i * 2 + 1)));
        }
        return;
    }
    switch (opcode) {
        case Opcode::CONST:
        case Opcode::NOP:
        case Opcode::RET_VOID:
        case Opcode::SWITCH:
        case Opcode::NUM_OPCODES:
            return;
        case Opcode::FUNARG:
            return;  // arg(0) is parameter index
        case Opcode::BR:
            return;  // arg(0) is block offset
        case Opcode::RET:
            fn(inst.arg(0));
            return;
        case Opcode::COND_BR:
            fn(inst.arg(0));  // condition; arg(1),arg(2) are block offsets
            return;
        case Opcode::CHECKED_ADD:
        case Opcode::CHECKED_SUB:
        case Opcode::CHECKED_MUL:
            fn(inst.arg(0));
            fn(inst.arg(1));  // arg(2),arg(3) are block offsets
            return;
        case Opcode::CALL0:
            return;  // arg(0) is callee ID
        case Opcode::CALL1:
        case Opcode::CALL2:
        case Opcode::CALL3:
        case Opcode::CALL4:
        case Opcode::CALL5:
        case Opcode::CALL6: {
            auto count = callParamCount(opcode);
            for (uint8_t i = 1; i <= count; ++i) {
                fn(inst.arg(i));
            }
            return;
        }
        default:
            for (uint8_t i = 0; i < inst.numArgs(); ++i) {
                fn(inst.arg(i));
            }
            return;
    }
}

}  // namespace umbra::ir
