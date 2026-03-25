#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

/// Live span information for a single IR value.
struct LiveSpan {
    uint32_t defPoint = 0;      ///< Sequence number where the value is defined.
    uint32_t lastUsePoint = 0;  ///< Sequence number of the last use.
};

/// LiveSpanAnalysis — Pass 0: RPO traversal with def/last-use tracking.
/// Computes live spans for all IR values in a function, enabling stack space
/// reuse and register allocation heuristics.
class LiveSpanAnalysis {
   public:
    /// Run the analysis on a function within the given program.
    void analyze(const ir::Program& program, ir::FunctionId func);

    /// Get the live span for a given instruction offset.
    /// Returns nullptr if not found (e.g., for constants that were folded).
    [[nodiscard]] const LiveSpan* getSpan(ir::IROp op) const;

    /// Check if a given instruction offset is the last use point at a given
    /// sequence number.
    [[nodiscard]] bool isLastUse(ir::IROp op, uint32_t seqNum) const;

    /// Get the RPO-ordered list of block offsets.
    [[nodiscard]] const std::vector<uint32_t>& rpoOrder() const { return rpoOrder_; }

    /// Get the sequence number for a given instruction offset.
    [[nodiscard]] uint32_t seqNum(ir::IROp op) const;

    /// Get the block that an instruction belongs to (block offset).
    [[nodiscard]] uint32_t blockOf(ir::IROp op) const;

   private:
    /// Compute RPO ordering of blocks starting from entryBlockOff.
    void computeRPO(const ir::Program& program, uint32_t entryBlockOff);

    /// Map from instruction offset -> live span.
    std::unordered_map<ir::IROp, LiveSpan> spans_;

    /// Map from instruction offset -> sequence number.
    std::unordered_map<ir::IROp, uint32_t> seqNums_;

    /// Map from instruction offset -> block offset it belongs to.
    std::unordered_map<ir::IROp, uint32_t> instBlock_;

    /// RPO-ordered block offsets.
    std::vector<uint32_t> rpoOrder_;
};

}  // namespace umbra::flying_start
