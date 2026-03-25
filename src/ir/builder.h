#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ir/block.h"
#include "ir/function.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::ir {

/// IRBuilder — the programmatic interface for constructing IR programs.
/// Supports constant deduplication, constant folding, and finalization with backpatching.
class IRBuilder {
   public:
    IRBuilder();

    // ── Program structure ─────────────────────────────────────────────

    /// Create a new function and return its ID.
    FunctionId createFunction(const std::string& name, TypeId returnType,
                              const std::vector<TypeId>& paramTypes);

    /// Create a new basic block within the current function.
    BlockId createBlock();

    /// Set the current insertion point to a block (starts writing into it).
    void setInsertPoint(BlockId block);

    /// Get the current block ID.
    [[nodiscard]] BlockId getCurrentBlock() const;

    /// Get the current function ID.
    [[nodiscard]] FunctionId getCurrentFunction() const;

    /// Set the current function ID (for restoring after host function registration).
    void setCurrentFunction(FunctionId func);

    // ── Constant and argument instructions ────────────────────────────

    /// Emit a constant instruction. Deduplicates: same (type,value) → same IROp.
    IROp emitConst(TypeId type, uint64_t value);

    /// Emit a function argument reference.
    IROp emitFunArg(TypeId type, uint32_t index);

    // ── Arithmetic ────────────────────────────────────────────────────

    IROp emitAdd(TypeId type, IROp lhs, IROp rhs);
    IROp emitSub(TypeId type, IROp lhs, IROp rhs);
    IROp emitMul(TypeId type, IROp lhs, IROp rhs);
    IROp emitDiv(TypeId type, IROp lhs, IROp rhs);
    IROp emitUDiv(TypeId type, IROp lhs, IROp rhs);
    IROp emitMod(TypeId type, IROp lhs, IROp rhs);
    IROp emitNeg(TypeId type, IROp operand);

    // ── Checked arithmetic ────────────────────────────────────────────

    IROp emitCheckedAdd(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow);
    IROp emitCheckedSub(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow);
    IROp emitCheckedMul(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow);

    // ── Bitwise ───────────────────────────────────────────────────────

    IROp emitAnd(TypeId type, IROp lhs, IROp rhs);
    IROp emitOr(TypeId type, IROp lhs, IROp rhs);
    IROp emitXor(TypeId type, IROp lhs, IROp rhs);
    IROp emitShl(TypeId type, IROp lhs, IROp rhs);
    IROp emitAShr(TypeId type, IROp lhs, IROp rhs);
    IROp emitLShr(TypeId type, IROp lhs, IROp rhs);
    IROp emitRotl(TypeId type, IROp lhs, IROp rhs);
    IROp emitRotr(TypeId type, IROp lhs, IROp rhs);

    // ── Comparison ────────────────────────────────────────────────────

    IROp emitCmpEq(TypeId type, IROp lhs, IROp rhs);
    IROp emitCmpNe(TypeId type, IROp lhs, IROp rhs);
    IROp emitCmpLt(TypeId type, IROp lhs, IROp rhs);
    IROp emitCmpLe(TypeId type, IROp lhs, IROp rhs);
    IROp emitCmpGt(TypeId type, IROp lhs, IROp rhs);
    IROp emitCmpGe(TypeId type, IROp lhs, IROp rhs);

    // ── Logical ───────────────────────────────────────────────────────

    IROp emitLNot(IROp operand);
    IROp emitLAnd(IROp lhs, IROp rhs);
    IROp emitLOr(IROp lhs, IROp rhs);
    IROp emitSelect(TypeId type, IROp cond, IROp trueVal, IROp falseVal);

    // ── Cast ──────────────────────────────────────────────────────────

    IROp emitSExt(TypeId targetType, IROp operand);
    IROp emitZExt(TypeId targetType, IROp operand);
    IROp emitTrunc(TypeId targetType, IROp operand);
    IROp emitBitcast(TypeId targetType, IROp operand);
    IROp emitPtr2Int(TypeId targetType, IROp operand);
    IROp emitInt2Ptr(IROp operand);

    // ── Memory ────────────────────────────────────────────────────────

    IROp emitLoad(TypeId type, IROp ptr);
    void emitStore(IROp val, IROp ptr);
    IROp emitGEP(IROp base, TypeId elemType, IROp index);

    // ── Control flow ──────────────────────────────────────────────────

    void emitBr(BlockId target);
    void emitCondBr(IROp cond, BlockId trueBB, BlockId falseBB);
    IROp emitRet(TypeId type, IROp val);
    void emitRetVoid();

    // ── Call ──────────────────────────────────────────────────────────

    /// Emit a call instruction. Selects the appropriate call variant based on arg count.
    IROp emitCall(TypeId retType, FunctionId callee, const std::vector<IROp>& args);

    // ── PHI ──────────────────────────────────────────────────────────

    /// Emit a PHI node. incomingList is a sequence of (BlockId, IROp) pairs.
    /// During construction, block IDs are temporary; finalize() will backpatch them.
    IROp emitPhi(TypeId type, const std::vector<std::pair<BlockId, IROp>>& incoming);

    /// Patch a PHI's incoming value (for back-edges in loops).
    void patchPhiValue(IROp phiOp, uint8_t incomingIndex, IROp newValue);

    /// Patch a PHI's incoming block (for back-edges in loops).
    void patchPhiBlock(IROp phiOp, uint8_t incomingIndex, BlockId newBlock);

    // ── DB-specific ──────────────────────────────────────────────────

    IROp emitIsNull(IROp operand);
    IROp emitCRC32(IROp data, IROp seed);
    IROp emitBSwap(TypeId type, IROp operand);

    // ── Finalization ─────────────────────────────────────────────────

    /// Finalize the IR: backpatch block IDs to byte offsets.
    /// Returns the completed Program. The builder should not be used after this.
    Program finalize();

    /// Run dead code elimination on the program.
    /// Must be called after finalize().
    void eliminateDeadCode(Program& program);

   private:
    /// Emit a raw instruction, append to current block.
    IROp emitRaw(Opcode opcode, TypeId type, const IROp* args, uint8_t argCount);

    /// Emit a binary instruction (2 args).
    IROp emitBinary(Opcode opcode, TypeId type, IROp lhs, IROp rhs);

    /// Try to constant-fold a binary operation.
    /// Returns true and sets result if folding succeeded.
    bool tryFoldBinary(Opcode opcode, TypeId type, IROp lhs, IROp rhs, IROp& result);

    /// Try to constant-fold a comparison.
    bool tryFoldComparison(Opcode opcode, TypeId type, IROp lhs, IROp rhs, IROp& result);

    /// Check if an IROp refers to a constant instruction.
    [[nodiscard]] bool isConst(IROp op) const;

    /// Read the constant value from a CONST instruction.
    [[nodiscard]] uint64_t getConstValue(IROp op) const;

    /// Read the TypeId from a CONST instruction.
    [[nodiscard]] TypeId getConstType(IROp op) const;

    /// Record a patch location for block ID → byte offset backpatching.
    void addPatch(uint32_t instOffset, uint8_t argIndex);

    // ── Builder state ────────────────────────────────────────────────

    /// The instruction buffer being built.
    std::vector<uint8_t> instBuffer_;
    /// The block buffer being built (array of instruction offsets).
    std::vector<uint32_t> blockBuffer_;
    /// Function entries (hot-path).
    std::vector<FunctionEntry> functions_;
    /// Function metadata (side table).
    std::vector<FunctionMeta> meta_;

    /// Current insertion block ID.
    BlockId currentBlock_ = INVALID_BLOCK;
    /// Current function ID.
    FunctionId currentFunction_ = UINT32_MAX;
    /// Next block ID.
    BlockId nextBlockId_ = 0;

    /// Block ID → byte offset in block buffer (filled when setInsertPoint is called).
    std::unordered_map<BlockId, uint32_t> blockOffsets_;

    /// Constant deduplication: (type, value) → IROp.
    std::unordered_map<uint64_t, IROp> constDedup_;

    /// Patch list: (instOffset, argIndex) pairs for block ID backpatching.
    struct PatchEntry {
        uint32_t instOffset;
        uint8_t argIndex;
    };
    std::vector<PatchEntry> patchList_;
};

}  // namespace umbra::ir
