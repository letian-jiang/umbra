#include "backend/flying_start/register_alloc.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <vector>

#include "ir/instruction.h"

namespace umbra::flying_start {

// ── Static register sets ─────────────────────────────────────────────────────
//
// Per the design doc (§5.2):
//   Callee-saved (long-lived values): RBX, R12-R15       (5 regs)
//   Caller-saved (temporaries):       R8-R11              (4 regs)
//   Scratch (intra-instruction):      RAX, RCX, RDX, RSI (4 regs)
//   Reserved:                         RSP, RBP, RDI       (3 regs)

namespace {

// Allocatable registers, caller-saved first (prefer these for short-lived).
const std::array<asmjit::x86::Gp, 9> ALLOCATABLE_REGS = {
    asmjit::x86::r8,  asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11, asmjit::x86::rbx,
    asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15,
};

const std::array<asmjit::x86::Gp, 4> SCRATCH_REGS = {
    asmjit::x86::rax,
    asmjit::x86::rcx,
    asmjit::x86::rdx,
    asmjit::x86::rsi,
};

const std::array<asmjit::x86::Gp, 5> CALLEE_SAVED = {
    asmjit::x86::rbx, asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15,
};

const std::array<asmjit::x86::Gp, 4> CALLER_SAVED = {
    asmjit::x86::r8,
    asmjit::x86::r9,
    asmjit::x86::r10,
    asmjit::x86::r11,
};

}  // namespace

// ── Constructor ──────────────────────────────────────────────────────────────

RegisterAllocator::RegisterAllocator() {
    // Initialize free register pool (reverse order so we pop caller-saved first).
    freeRegs_.assign(ALLOCATABLE_REGS.rbegin(), ALLOCATABLE_REGS.rend());
}

// ── Register allocation ──────────────────────────────────────────────────────

std::optional<asmjit::x86::Gp> RegisterAllocator::allocateRegister() {
    if (freeRegs_.empty()) {
        return std::nullopt;
    }

    auto reg = freeRegs_.back();
    freeRegs_.pop_back();
    usedRegs_.push_back(reg);
    markCalleeSaved(reg);
    return reg;
}

void RegisterAllocator::freeRegister(const asmjit::x86::Gp& reg) {
    auto it = std::find_if(usedRegs_.begin(), usedRegs_.end(),
                           [&](const asmjit::x86::Gp& r) { return r.id() == reg.id(); });
    if (it != usedRegs_.end()) {
        usedRegs_.erase(it);
        freeRegs_.push_back(reg);
    }
}

bool RegisterAllocator::hasAvailableRegister() const {
    return !freeRegs_.empty();
}

int RegisterAllocator::availableRegisterCount() const {
    return static_cast<int>(freeRegs_.size());
}

// ── Stack slot allocation ────────────────────────────────────────────────────

int32_t RegisterAllocator::allocateStackSlot() {
    if (!freeSlots_.empty()) {
        int32_t slot = freeSlots_.back();
        freeSlots_.pop_back();
        return slot;
    }

    int32_t offset = nextStackOffset_;
    nextStackOffset_ -= 8;
    auto used = static_cast<int32_t>(-offset);
    if (used > maxStackUsed_) {
        maxStackUsed_ = used;
    }
    return offset;
}

void RegisterAllocator::freeStackSlot(int32_t offset) {
    freeSlots_.push_back(offset);
}

int32_t RegisterAllocator::spillToStack(const asmjit::x86::Gp& reg) {
    int32_t offset = allocateStackSlot();
    freeRegister(reg);
    return offset;
}

// ── Callee-saved tracking ────────────────────────────────────────────────────

void RegisterAllocator::markCalleeSaved(const asmjit::x86::Gp& reg) {
    if (!isCalleeSaved(reg)) {
        return;
    }
    auto it = std::find_if(usedCalleeSaved_.begin(), usedCalleeSaved_.end(),
                           [&](const asmjit::x86::Gp& r) { return r.id() == reg.id(); });
    if (it == usedCalleeSaved_.end()) {
        usedCalleeSaved_.push_back(reg);
    }
}

bool RegisterAllocator::isCalleeSaved(const asmjit::x86::Gp& reg) {
    return std::any_of(CALLEE_SAVED.begin(), CALLEE_SAVED.end(),
                       [&](const asmjit::x86::Gp& r) { return r.id() == reg.id(); });
}

bool RegisterAllocator::isCallerSaved(const asmjit::x86::Gp& reg) {
    return std::any_of(CALLER_SAVED.begin(), CALLER_SAVED.end(),
                       [&](const asmjit::x86::Gp& r) { return r.id() == reg.id(); });
}

std::vector<asmjit::x86::Gp> RegisterAllocator::liveCallerSaved() const {
    std::vector<asmjit::x86::Gp> result;
    for (const auto& reg : usedRegs_) {
        if (isCallerSaved(reg)) {
            result.push_back(reg);
        }
    }
    return result;
}

// ── Scratch registers ────────────────────────────────────────────────────────

asmjit::x86::Gp RegisterAllocator::scratchReg(int idx) {
    assert(idx >= 0 && idx < NUM_SCRATCH);
    return SCRATCH_REGS[static_cast<size_t>(idx)];
}

// ── Value tracking ───────────────────────────────────────────────────────────

void RegisterAllocator::setLocation(ir::IROp op, const ValueLocation& loc) {
    locations_[op] = loc;
}

const ValueLocation* RegisterAllocator::getLocation(ir::IROp op) const {
    auto it = locations_.find(op);
    if (it != locations_.end()) {
        return &it->second;
    }
    return nullptr;
}

void RegisterAllocator::removeLocation(ir::IROp op) {
    auto it = locations_.find(op);
    if (it != locations_.end()) {
        auto& loc = it->second;
        if (loc.kind == LocationKind::REGISTER) {
            freeRegister(loc.reg);
        } else if (loc.kind == LocationKind::STACK) {
            freeStackSlot(loc.stackOffset);
        }
        locations_.erase(it);
    }
}

ir::IROp RegisterAllocator::findValueInReg(const asmjit::x86::Gp& reg) const {
    for (const auto& [op, loc] : locations_) {
        if (loc.kind == LocationKind::REGISTER && loc.reg.id() == reg.id()) {
            return op;
        }
    }
    return ir::INVALID_OP;
}

}  // namespace umbra::flying_start
