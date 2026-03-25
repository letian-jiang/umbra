#include "backend/flying_start/live_span.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

// ── RPO computation ──────────────────────────────────────────────────────────

namespace {

/// Collect instruction offsets from a block, stopping at (and including)
/// the terminator.
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

/// Get successor block offsets from a block's terminator.
std::vector<uint32_t> getSuccessors(const ir::Program& program, uint32_t blockOff) {
    std::vector<uint32_t> successors;
    auto insts = collectBlockInsts(program, blockOff);
    if (insts.empty()) {
        return successors;
    }

    ir::InstructionView terminator(program.instBuffer().data(), insts.back());
    auto opcode = terminator.opcode();

    switch (opcode) {
        case ir::Opcode::BR:
            successors.push_back(terminator.arg(0));
            break;
        case ir::Opcode::COND_BR:
            successors.push_back(terminator.arg(1));  // true branch
            successors.push_back(terminator.arg(2));  // false branch
            break;
        case ir::Opcode::CHECKED_ADD:
        case ir::Opcode::CHECKED_SUB:
        case ir::Opcode::CHECKED_MUL:
            successors.push_back(terminator.arg(2));  // continue
            successors.push_back(terminator.arg(3));  // overflow
            break;
        default:
            // RET, RET_VOID, and all other opcodes: no successors.
            break;
    }
    return successors;
}

}  // namespace

void LiveSpanAnalysis::computeRPO(const ir::Program& program, uint32_t entryBlockOff) {
    rpoOrder_.clear();

    // DFS post-order, then reverse.
    std::vector<uint32_t> postOrder;
    std::unordered_set<uint32_t> visited;
    std::vector<std::pair<uint32_t, size_t>> stack;  // (blockOff, nextSuccIdx)

    stack.emplace_back(entryBlockOff, 0);
    visited.insert(entryBlockOff);

    while (!stack.empty()) {
        auto& [blockOff, idx] = stack.back();
        auto succs = getSuccessors(program, blockOff);

        if (idx < succs.size()) {
            uint32_t succ = succs[idx];
            ++idx;
            if (visited.find(succ) == visited.end()) {
                visited.insert(succ);
                stack.emplace_back(succ, 0);
            }
        } else {
            postOrder.push_back(blockOff);
            stack.pop_back();
        }
    }

    // Reverse post-order.
    rpoOrder_.assign(postOrder.rbegin(), postOrder.rend());
}

// ── Live span analysis ───────────────────────────────────────────────────────

void LiveSpanAnalysis::analyze(const ir::Program& program, ir::FunctionId func) {
    const auto& entry = program.functionEntry(func);
    assert(entry.entryBlockOffset != ir::INVALID_BLOCK);

    computeRPO(program, entry.entryBlockOffset);

    spans_.clear();
    seqNums_.clear();
    instBlock_.clear();

    uint32_t seq = 0;

    // First pass: assign sequence numbers and record def points.
    for (uint32_t blockOff : rpoOrder_) {
        auto insts = collectBlockInsts(program, blockOff);
        for (uint32_t instOff : insts) {
            seqNums_[instOff] = seq;
            instBlock_[instOff] = blockOff;

            LiveSpan span;
            span.defPoint = seq;
            span.lastUsePoint = seq;  // Default: last use = def (unused value)
            spans_[instOff] = span;

            // Update last-use points for all operands.
            ir::InstructionView inst(program.instBuffer().data(), instOff);
            auto opcode = inst.opcode();

            if (ir::isPhi(opcode)) {
                // PHI: each incoming value's last-use extends to the end of
                // its predecessor block. We handle this by extending the
                // last-use to at least the current seq.
                auto count = ir::phiIncomingCount(opcode);
                for (uint8_t i = 0; i < count; ++i) {
                    uint32_t incomingValue = inst.arg(static_cast<uint8_t>(i * 2 + 1));
                    auto it = spans_.find(incomingValue);
                    if (it != spans_.end()) {
                        if (seq > it->second.lastUsePoint) {
                            it->second.lastUsePoint = seq;
                        }
                    }
                }
            } else {
                auto numArgs = inst.numArgs();
                for (uint8_t i = 0; i < numArgs; ++i) {
                    uint32_t argOff = inst.arg(i);
                    auto it = spans_.find(argOff);
                    if (it != spans_.end()) {
                        if (seq > it->second.lastUsePoint) {
                            it->second.lastUsePoint = seq;
                        }
                    }
                }
            }

            ++seq;
        }
    }
}

const LiveSpan* LiveSpanAnalysis::getSpan(ir::IROp op) const {
    auto it = spans_.find(op);
    if (it != spans_.end()) {
        return &it->second;
    }
    return nullptr;
}

bool LiveSpanAnalysis::isLastUse(ir::IROp op, uint32_t seqNumber) const {
    auto it = spans_.find(op);
    if (it != spans_.end()) {
        return it->second.lastUsePoint == seqNumber;
    }
    return false;
}

uint32_t LiveSpanAnalysis::seqNum(ir::IROp op) const {
    auto it = seqNums_.find(op);
    assert(it != seqNums_.end());
    return it->second;
}

uint32_t LiveSpanAnalysis::blockOf(ir::IROp op) const {
    auto it = instBlock_.find(op);
    assert(it != instBlock_.end());
    return it->second;
}

}  // namespace umbra::flying_start
