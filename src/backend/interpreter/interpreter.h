#pragma once

#include <cstdint>
#include <functional>
#include <ostream>
#include <span>
#include <utility>
#include <vector>

#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::interpreter {

/// Interpreter — reference execution engine for Umbra IR.
/// Executes IR programs by interpreting instructions one by one.
/// Correctness-first design; no performance optimizations.
class Interpreter {
   public:
    /// Execute a function in the program with the given arguments.
    /// Returns the function's return value as a uint64_t.
    uint64_t execute(ir::Program& program, ir::FunctionId func, std::span<const uint64_t> args);

    /// Enable/disable instruction-level trace output.
    void setTraceEnabled(bool enabled) { traceEnabled_ = enabled; }

    /// Set the output stream for trace output (default: stderr).
    void setTraceOutput(std::ostream* out) { traceOut_ = out; }

   private:
    /// Result of executing a single instruction.
    struct ExecuteResult {
        enum Type { CONTINUE, BRANCH, RETURN };
        Type type;
        uint32_t targetBlockOff;  // for BRANCH: block buffer offset of target
        uint64_t returnValue;     // for RETURN: the return value

        static ExecuteResult makeContinue() { return {CONTINUE, 0, 0}; }
        static ExecuteResult makeBranch(uint32_t target) { return {BRANCH, target, 0}; }
        static ExecuteResult makeReturn(uint64_t val) { return {RETURN, 0, val}; }
    };

    /// Execute starting from a block, returning the function's return value.
    uint64_t executeFrom(ir::Program& program, uint32_t blockOff);

    /// Initialize function arguments in the value slots.
    void initFunArgs(ir::Program& program, uint32_t entryBlockOff, std::span<const uint64_t> args);

    /// Resolve PHI nodes at the beginning of a block.
    /// Returns the position (block buffer index) of the first non-PHI instruction.
    uint32_t resolvePhis(ir::Program& program, uint32_t blockOff, uint32_t prevBlockOff);

    /// Execute a single non-PHI instruction. Returns the execution result.
    ExecuteResult executeInst(ir::Program& program, const ir::InstructionView& inst,
                              uint32_t instOff);

    /// Call a host function through manual dispatch.
    uint64_t callHostFunction(const ir::HostFunction& func, std::span<const uint64_t> args);

    /// Trace a single instruction execution.
    void traceInst(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);

    /// Value slots indexed by instruction offset.
    std::vector<uint64_t> slots_;

    /// Trace mode.
    bool traceEnabled_ = false;
    std::ostream* traceOut_ = nullptr;

    /// Sentinel for "no predecessor" (entry block on first entry).
    static constexpr uint32_t INVALID_PREV = UINT32_MAX;
};

}  // namespace umbra::interpreter
