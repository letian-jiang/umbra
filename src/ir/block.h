#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "ir/instruction.h"
#include "ir/opcode.h"

namespace umbra::ir {

/// BlockId — temporary identifier used during IR construction.
/// After finalize(), blocks are referenced by byte offset into the block buffer.
using BlockId = uint32_t;

/// Sentinel for invalid block.
constexpr BlockId INVALID_BLOCK = UINT32_MAX;

/// BlockView — iterates instructions of a block stored in the block buffer.
/// A block is a sequence of uint32_t instruction offsets in the block buffer,
/// terminated when the last instruction is a terminator opcode.
class BlockView {
   public:
    BlockView(const std::vector<uint32_t>& blockBuffer, const std::vector<uint8_t>& instBuffer,
              uint32_t blockOffset)
        : blockBuffer_(blockBuffer), instBuffer_(instBuffer), blockOffset_(blockOffset) {}

    /// Iterator over instructions in this block.
    class Iterator {
       public:
        Iterator(const std::vector<uint32_t>& blockBuffer, const std::vector<uint8_t>& instBuffer,
                 uint32_t pos, bool atEnd)
            : blockBuffer_(blockBuffer), instBuffer_(instBuffer), pos_(pos), atEnd_(atEnd) {}

        InstructionView operator*() const { return {instBuffer_.data(), blockBuffer_[pos_]}; }

        Iterator& operator++() {
            // Check if current instruction is a terminator
            auto instOff = blockBuffer_[pos_];
            auto opcode = static_cast<Opcode>(instBuffer_[instOff]);
            ++pos_;
            if (isTerminator(opcode)) {
                atEnd_ = true;
            }
            return *this;
        }

        bool operator!=(const Iterator& other) const {
            if (atEnd_ && other.atEnd_) {
                return false;
            }
            return atEnd_ != other.atEnd_ || pos_ != other.pos_;
        }

       private:
        const std::vector<uint32_t>& blockBuffer_;
        const std::vector<uint8_t>& instBuffer_;
        uint32_t pos_;
        bool atEnd_;
    };

    [[nodiscard]] Iterator begin() const {
        return {blockBuffer_, instBuffer_, blockOffset_, false};
    }
    [[nodiscard]] Iterator end() const { return {blockBuffer_, instBuffer_, 0, true}; }

    /// Get the instruction offset at position i within this block.
    [[nodiscard]] IROp instructionAt(uint32_t i) const { return blockBuffer_[blockOffset_ + i]; }

    /// Get the byte offset in the block buffer where this block starts.
    [[nodiscard]] uint32_t offset() const { return blockOffset_; }

   private:
    const std::vector<uint32_t>& blockBuffer_;
    const std::vector<uint8_t>& instBuffer_;
    uint32_t blockOffset_;
};

}  // namespace umbra::ir
