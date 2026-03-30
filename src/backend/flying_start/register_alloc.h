#pragma once

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <array>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "ir/instruction.h"

namespace umbra::flying_start {

/// Value location types for the JIT compiler.
enum class LocationKind : uint8_t {
    NONE,      ///< Not yet assigned.
    REGISTER,  ///< In a machine register.
    STACK,     ///< On the stack at [rbp - offset].
    CONST,     ///< Constant value (inline into instructions).
    FLAGS,     ///< Result is in CPU flags (deferred comparison).
    DEFERRED,  ///< Translation deferred (e.g., GEP lazy evaluation).
};

/// A value's location in the generated code.
struct ValueLocation {
    LocationKind kind = LocationKind::NONE;
    asmjit::x86::Gp reg;                        ///< Valid when kind == REGISTER.
    int32_t stackOffset = 0;                    ///< Valid when kind == STACK. Offset from RBP.
    uint64_t constValue = 0;                    ///< Valid when kind == CONST.
    uint8_t conditionCode = 0;                  ///< Valid when kind == FLAGS (x86 condition code).
    ir::IROp deferredOp = 0;                    ///< Valid when kind == DEFERRED.
    ir::IROp gepBase = ir::INVALID_OP;          ///< Deferred GEP base pointer.
    ir::IROp gepIndex = ir::INVALID_OP;         ///< Deferred GEP index operand.
    ir::TypeId gepElemType = ir::TypeId::VOID;  ///< Deferred GEP element type.
    ir::TypeId type = ir::TypeId::VOID;         ///< The IR type of this value.
};

/// RegisterAllocator — 9-register allocation heuristic.
/// Manages machine register and stack slot assignment for IR values.
class RegisterAllocator {
   public:
    RegisterAllocator();

    /// Try to allocate a register for a value.
    /// Returns the register if successful, nullopt if all registers are in use.
    std::optional<asmjit::x86::Gp> allocateRegister();

    /// Free a register back to the pool.
    void freeRegister(const asmjit::x86::Gp& reg);

    /// Allocate a stack slot. Returns the offset from RBP (negative).
    int32_t allocateStackSlot();

    /// Free a stack slot for reuse.
    void freeStackSlot(int32_t offset);

    /// Get the current high-water mark for stack allocation (max frame size).
    [[nodiscard]] int32_t maxStackSize() const { return maxStackUsed_; }

    /// Check if any registers are available.
    [[nodiscard]] bool hasAvailableRegister() const;

    /// Get the number of available registers.
    [[nodiscard]] int availableRegisterCount() const;

    /// Spill a value from register to stack. Returns the new stack offset.
    /// The caller is responsible for emitting the actual mov instruction.
    int32_t spillToStack(const asmjit::x86::Gp& reg);

    /// Get the set of callee-saved registers that have been used.
    [[nodiscard]] const std::vector<asmjit::x86::Gp>& usedCalleeSaved() const {
        return usedCalleeSaved_;
    }

    /// Get the set of caller-saved registers that currently hold live values.
    [[nodiscard]] std::vector<asmjit::x86::Gp> liveCallerSaved() const;

    // ── Scratch register management ──────────────────────────────────────

    /// The four scratch registers: RAX, RCX, RDX, RSI.
    static constexpr int NUM_SCRATCH = 4;

    /// Get a scratch register by index.
    [[nodiscard]] static asmjit::x86::Gp scratchReg(int idx);

    // ── Value tracking ───────────────────────────────────────────────────

    /// Record that an IR value lives in the given location.
    void setLocation(ir::IROp op, const ValueLocation& loc);

    /// Look up where an IR value currently lives.
    [[nodiscard]] const ValueLocation* getLocation(ir::IROp op) const;

    /// Remove tracking for an IR value (when its live span ends).
    void removeLocation(ir::IROp op);

    /// Find which IR value (if any) currently occupies the given register.
    [[nodiscard]] ir::IROp findValueInReg(const asmjit::x86::Gp& reg) const;

   private:
    /// Mark a callee-saved register as used (for prologue/epilogue).
    void markCalleeSaved(const asmjit::x86::Gp& reg);

    /// Is this register callee-saved?
    [[nodiscard]] static bool isCalleeSaved(const asmjit::x86::Gp& reg);

    /// Is this register caller-saved (allocatable, not scratch)?
    [[nodiscard]] static bool isCallerSaved(const asmjit::x86::Gp& reg);

    /// The 9 allocatable registers (5 callee-saved + 4 caller-saved).
    static constexpr int NUM_ALLOCATABLE = 9;

    /// Pool of available registers.
    std::vector<asmjit::x86::Gp> freeRegs_;

    /// Set of registers currently in use.
    std::vector<asmjit::x86::Gp> usedRegs_;

    /// Callee-saved registers that have been allocated (need save/restore).
    std::vector<asmjit::x86::Gp> usedCalleeSaved_;

    /// Stack slot allocation.
    int32_t nextStackOffset_ = -8;    ///< Next available offset from RBP.
    int32_t maxStackUsed_ = 0;        ///< High-water mark (positive value).
    std::vector<int32_t> freeSlots_;  ///< Freed slots available for reuse.

    /// Value location map: IROp -> location.
    std::unordered_map<ir::IROp, ValueLocation> locations_;
};

}  // namespace umbra::flying_start
