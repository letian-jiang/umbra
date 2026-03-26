#pragma once

#include <asmjit/core.h>
#include <asmjit/x86.h>

#include <cstdint>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "backend/flying_start/live_span.h"
#include "backend/flying_start/register_alloc.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

/// FlyingStartCompiler — the JIT compiler that translates IR to x86-64.
/// Implements the Flying Start approach from the Tidy Tuples paper:
///   Pass 0: Live span analysis (done by LiveSpanAnalysis)
///   Pass 1: Direct translation with 4 on-the-fly optimizations
class FlyingStartCompiler {
   public:
    FlyingStartCompiler();
    ~FlyingStartCompiler();

    // Non-copyable, non-movable (owns asmjit resources).
    FlyingStartCompiler(const FlyingStartCompiler&) = delete;
    FlyingStartCompiler& operator=(const FlyingStartCompiler&) = delete;
    FlyingStartCompiler(FlyingStartCompiler&&) = delete;
    FlyingStartCompiler& operator=(FlyingStartCompiler&&) = delete;

    /// Compile a function in the program. Returns the function pointer.
    /// The function signature is: uint64_t (*)(uint64_t arg0, uint64_t arg1, ...)
    /// For pipeline functions: void (*)(RuntimeContext*)
    void* compile(ir::Program& program, ir::FunctionId func);

    /// Compile a function and capture the x86-64 assembly as text.
    /// Returns the function pointer. Assembly text is appended to asmOutput.
    void* compileWithLog(ir::Program& program, ir::FunctionId func, std::string& asmOutput);

    /// Execute a compiled function with arguments.
    static uint64_t execute(void* funcPtr, std::span<const uint64_t> args);

    /// Compile and execute in one step (convenience).
    uint64_t compileAndExecute(ir::Program& program, ir::FunctionId func,
                               std::span<const uint64_t> args);

   private:
    // ── Translation dispatch ─────────────────────────────────────────────

    /// Translate all blocks in RPO order.
    void translateFunction(ir::Program& program, ir::FunctionId func);

    /// Translate a single block.
    void translateBlock(ir::Program& program, uint32_t blockOff);

    /// Translate a single instruction.
    void translateInstruction(ir::Program& program, const ir::InstructionView& inst,
                              uint32_t instOff);

    // ── Opcode translation methods ───────────────────────────────────────

    void translateConst(const ir::InstructionView& inst, uint32_t instOff);
    void translateFunArg(const ir::InstructionView& inst, uint32_t instOff);
    void translateAdd(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateSub(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateMul(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateDiv(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateUDiv(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateMod(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateNeg(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateCheckedArith(ir::Program& program, const ir::InstructionView& inst,
                               uint32_t instOff);
    void translateBitwiseAnd(ir::Program& program, const ir::InstructionView& inst,
                             uint32_t instOff);
    void translateBitwiseOr(ir::Program& program, const ir::InstructionView& inst,
                            uint32_t instOff);
    void translateBitwiseXor(ir::Program& program, const ir::InstructionView& inst,
                             uint32_t instOff);
    void translateShl(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateAShr(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateLShr(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateRotl(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateRotr(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateCmp(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateLNot(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateLAnd(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateLOr(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateSelect(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateSExt(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateZExt(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateTrunc(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateBitcast(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateLoad(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateStore(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateGep(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateBr(ir::Program& program, const ir::InstructionView& inst);
    void translateCondBr(ir::Program& program, const ir::InstructionView& inst);
    void translateRet(ir::Program& program, const ir::InstructionView& inst);
    void translateRetVoid();
    void translateCall(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateIsNull(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateCrc32(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);
    void translateBSwap(ir::Program& program, const ir::InstructionView& inst, uint32_t instOff);

    // ── PHI resolution ───────────────────────────────────────────────────

    /// Resolve PHIs when branching to targetBlock from currentBlock.
    void resolvePhis(ir::Program& program, uint32_t targetBlockOff);

    // ── Value access helpers ─────────────────────────────────────────────

    /// Load an IR value into a register and return it.
    /// If the value is already in a register, returns that register.
    /// If in stack/const, emits a load and returns the scratch register used.
    asmjit::x86::Gp loadValueToReg(ir::IROp op, const asmjit::x86::Gp& hint);

    /// Ensure a value is in a register suitable for being the destination
    /// of an in-place operation (like add). Returns the register.
    asmjit::x86::Gp loadValueForModify(ir::IROp op, const asmjit::x86::Gp& hint);

    /// Store a result from a register to the result location for instOff.
    void storeResult(uint32_t instOff, const asmjit::x86::Gp& reg, ir::TypeId type);

    /// Decide where to place a result (register or stack) and record it.
    ValueLocation assignResultLocation(uint32_t instOff, ir::TypeId type);

    /// Emit comparison instruction between two operands.
    void emitComparison(ir::IROp lhsOp, ir::IROp rhsOp);

    /// Return true if the operand is a constant IR value.
    [[nodiscard]] static bool isConstOp(const ir::Program& program, ir::IROp op);

    /// Read the constant payload from a CONST instruction.
    [[nodiscard]] static uint64_t getConstValue(const ir::Program& program, ir::IROp op);

    struct SignedMagic {
        int64_t magic;
        uint8_t shift;
    };

    /// Compute signed-division magic for a positive constant divisor.
    [[nodiscard]] static SignedMagic computeSignedMagic(int64_t divisor);

    /// Free operand locations if this is their last use.
    void freeIfLastUse(ir::IROp op, uint32_t seqNum);

    /// Free all operand locations that reached their last use at this instruction.
    void freeLastUseOperands(const ir::InstructionView& inst, uint32_t instOff);

    // ── Prologue / Epilogue ──────────────────────────────────────────────

    void emitPrologue();
    void emitEpilogue();

    // ── State ────────────────────────────────────────────────────────────

    /// asmjit resources.
    asmjit::JitRuntime runtime_;
    asmjit::CodeHolder* code_ = nullptr;
    asmjit::x86::Assembler* as_ = nullptr;

    /// Analysis results.
    LiveSpanAnalysis lsa_;

    /// Register allocator.
    RegisterAllocator regAlloc_;

    /// Block labels.
    std::unordered_map<uint32_t, asmjit::Label> blockLabels_;

    /// Label for the epilogue (shared return path).
    asmjit::Label epilogueLabel_;

    /// Current block offset.
    uint32_t currentBlockOff_ = 0;

    /// Current sequence number within the translation.
    uint32_t currentSeq_ = 0;

    /// Location to backpatch the frame size in the prologue.
    size_t frameSizePatchOffset_ = 0;

    /// Number of function parameters.
    uint32_t numParams_ = 0;

    /// Pointer to the current program (for host function lookups during calls).
    ir::Program* currentProgram_ = nullptr;
};

}  // namespace umbra::flying_start
