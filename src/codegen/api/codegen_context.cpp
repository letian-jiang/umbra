#include "codegen/api/codegen_context.h"

#include <cassert>
#include <utility>

namespace umbra::codegen {

// ── Thread-local active context for operator overloads ──────────────────────

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
namespace {
thread_local CodegenContext* activeCtx = nullptr;
}  // namespace
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

void setActiveContext(CodegenContext* ctx) {
    activeCtx = ctx;
}

CodegenContext& activeContext() {
    assert(activeCtx != nullptr && "No active CodegenContext set");
    return *activeCtx;
}

// ── CodegenContext ──────────────────────────────────────────────────────────

CodegenContext::CodegenContext() = default;

ir::FunctionId CodegenContext::createFunction(const std::string& name, ir::TypeId returnType,
                                              const std::vector<ir::TypeId>& paramTypes) {
    return builder_.createFunction(name, returnType, paramTypes);
}

ir::BlockId CodegenContext::createBlock() {
    return builder_.createBlock();
}

void CodegenContext::setInsertPoint(ir::BlockId block) {
    builder_.setInsertPoint(block);
}

ir::BlockId CodegenContext::getCurrentBlock() const {
    return builder_.getCurrentBlock();
}

Bool CodegenContext::constBool(bool value) {
    return {builder_.emitConst(ir::TypeId::BOOL, value ? 1 : 0)};
}

Int8 CodegenContext::constInt8(int8_t value) {
    return {
        builder_.emitConst(ir::TypeId::INT8, static_cast<uint64_t>(static_cast<int64_t>(value)))};
}

Int32 CodegenContext::constInt32(int32_t value) {
    return {
        builder_.emitConst(ir::TypeId::INT32, static_cast<uint64_t>(static_cast<int64_t>(value)))};
}

Int64 CodegenContext::constInt64(int64_t value) {
    return {builder_.emitConst(ir::TypeId::INT64, static_cast<uint64_t>(value))};
}

UInt64 CodegenContext::constUInt64(uint64_t value) {
    return {builder_.emitConst(ir::TypeId::UINT64, value)};
}

void CodegenContext::emitBr(ir::BlockId target) {
    builder_.emitBr(target);
}

void CodegenContext::emitCondBr(Bool cond, ir::BlockId trueBB, ir::BlockId falseBB) {
    builder_.emitCondBr(cond.op(), trueBB, falseBB);
}

void CodegenContext::emitRetVoid() {
    builder_.emitRetVoid();
}

Bool CodegenContext::lnot(Bool operand) {
    return {builder_.emitLNot(operand.op())};
}

Bool CodegenContext::land(Bool lhs, Bool rhs) {
    return {builder_.emitLAnd(lhs.op(), rhs.op())};
}

Bool CodegenContext::lor(Bool lhs, Bool rhs) {
    return {builder_.emitLOr(lhs.op(), rhs.op())};
}

ir::FunctionId CodegenContext::registerHostFunction(const std::string& name, ir::HostFunction func,
                                                    ir::TypeId returnType,
                                                    const std::vector<ir::TypeId>& paramTypes) {
    return registerHostFunction(name, std::move(func), returnType, paramTypes, nullptr, false);
}

ir::FunctionId CodegenContext::registerHostFunction(const std::string& name, ir::HostFunction func,
                                                    ir::TypeId returnType,
                                                    const std::vector<ir::TypeId>& paramTypes,
                                                    void* directPtr, bool inlineable) {
    // Save current insertion point, since createFunction changes it.
    auto savedBlock = builder_.getCurrentBlock();
    auto savedFunction = builder_.getCurrentFunction();

    // We register on the builder side so the function gets an ID in the function table.
    // But the actual HostFunction is registered on the Program after finalize().
    auto id = builder_.createFunction(name, returnType, paramTypes);
    hostFuncs_.push_back({id, std::move(func), directPtr, inlineable});

    // Restore insertion point to the original function/block.
    builder_.setCurrentFunction(savedFunction);
    if (savedBlock != ir::IR_INVALID_BLOCK) {
        builder_.setInsertPoint(savedBlock);
    }

    return id;
}

void CodegenContext::patchPhiValue(ir::IROp phiOp, uint8_t incomingIndex, ir::IROp newValue) {
    builder_.patchPhiValue(phiOp, incomingIndex, newValue);
}

void CodegenContext::patchPhiBlock(ir::IROp phiOp, uint8_t incomingIndex, ir::BlockId newBlock) {
    builder_.patchPhiBlock(phiOp, incomingIndex, newBlock);
}

ir::Program CodegenContext::finalize() {
    auto program = builder_.finalize();
    // Register all host functions on the finalized program.
    for (auto& entry : hostFuncs_) {
        program.registerHostFunction(entry.id, std::move(entry.func));
        auto& meta = program.functionMetaMut(entry.id);
        meta.directPtr = entry.directPtr;
        meta.inlineable = entry.inlineable;
    }
    hostFuncs_.clear();
    return program;
}

// ── Operator overloads: Int32 ───────────────────────────────────────────────

Int32 operator+(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitAdd(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Int32 operator-(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitSub(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Int32 operator*(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitMul(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Int32 operator/(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitDiv(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator==(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpEq(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator!=(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpNe(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator<(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpLt(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator<=(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpLe(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator>(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpGt(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

Bool operator>=(Int32 lhs, Int32 rhs) {
    return {activeContext().builder().emitCmpGe(ir::TypeId::INT32, lhs.op(), rhs.op())};
}

// ── Operator overloads: Int64 ───────────────────────────────────────────────

Int64 operator+(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitAdd(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Int64 operator-(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitSub(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Int64 operator*(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitMul(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Int64 operator/(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitDiv(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator==(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpEq(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator!=(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpNe(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator<(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpLt(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator<=(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpLe(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator>(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpGt(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

Bool operator>=(Int64 lhs, Int64 rhs) {
    return {activeContext().builder().emitCmpGe(ir::TypeId::INT64, lhs.op(), rhs.op())};
}

// ── Operator overloads: UInt64 ──────────────────────────────────────────────

UInt64 operator+(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitAdd(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

UInt64 operator-(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitSub(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

UInt64 operator*(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitMul(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

UInt64 operator/(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitUDiv(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator==(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpEq(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator!=(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpNe(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator<(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpLt(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator<=(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpLe(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator>(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpGt(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

Bool operator>=(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitCmpGe(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

UInt64 operator^(UInt64 lhs, UInt64 rhs) {
    return {activeContext().builder().emitXor(ir::TypeId::UINT64, lhs.op(), rhs.op())};
}

// ── IfBuilder ───────────────────────────────────────────────────────────────

IfBuilder::IfBuilder(CodegenContext& ctx, Bool cond)
    : ctx_(ctx),
      thenBlock_(ctx.createBlock()),
      elseBlock_(ctx.createBlock()),
      mergeBlock_(ctx.createBlock()),
      thenEndBlock_(ir::IR_INVALID_BLOCK),
      elseEndBlock_(ir::IR_INVALID_BLOCK) {
    // Emit conditional branch from current block to then/else.
    ctx_.emitCondBr(cond, thenBlock_, elseBlock_);
    // Start emitting into the then-block.
    ctx_.setInsertPoint(thenBlock_);
}

void IfBuilder::elseBlock() {
    hasElse_ = true;
    thenEndBlock_ = ctx_.getCurrentBlock();
    // Branch from end of then-block to merge.
    ctx_.emitBr(mergeBlock_);
    // Start emitting into the else-block.
    ctx_.setInsertPoint(elseBlock_);
}

void IfBuilder::done() {
    if (hasElse_) {
        elseEndBlock_ = ctx_.getCurrentBlock();
        ctx_.emitBr(mergeBlock_);
    } else {
        // No else block: then goes to merge, else goes directly to merge.
        thenEndBlock_ = ctx_.getCurrentBlock();
        ctx_.emitBr(mergeBlock_);
        // Else block just branches to merge (empty else).
        ctx_.setInsertPoint(elseBlock_);
        elseEndBlock_ = elseBlock_;
        ctx_.emitBr(mergeBlock_);
    }
    ctx_.setInsertPoint(mergeBlock_);
}

// ── LoopBuilder ─────────────────────────────────────────────────────────────

LoopBuilder::LoopBuilder(CodegenContext& ctx, const std::vector<ir::IROp>& loopVarInits,
                         const std::vector<ir::TypeId>& loopVarTypes)
    : ctx_(ctx), varTypes_(loopVarTypes) {
    assert(loopVarInits.size() == loopVarTypes.size());

    preheaderBlock_ = ctx_.getCurrentBlock();
    headerBlock_ = ctx_.createBlock();
    bodyBlock_ = ctx_.createBlock();
    exitBlock_ = ctx_.createBlock();

    // Branch from preheader to header.
    ctx_.emitBr(headerBlock_);
    ctx_.setInsertPoint(headerBlock_);

    // Create PHI nodes for loop variables.
    // Initial incoming: (preheaderBlock, initVal).
    // Back-edge incoming: will be patched in done().
    for (size_t i = 0; i < loopVarInits.size(); ++i) {
        auto phiOp = ctx_.builder().emitPhi(loopVarTypes[i], {{preheaderBlock_, loopVarInits[i]},
                                                              {ir::IR_INVALID_BLOCK, ir::INVALID_OP}});
        phiOps_.push_back(phiOp);
    }
    // Note: the loop body should check the condition and branch.
    // We go straight to the body; the user calls done() with the continue condition.
    ctx_.emitBr(bodyBlock_);
    ctx_.setInsertPoint(bodyBlock_);
}

void LoopBuilder::done(Bool continueCondition, const std::vector<ir::IROp>& updatedVars) {
    assert(updatedVars.size() == phiOps_.size());

    auto bodyEndBlock = ctx_.getCurrentBlock();

    // Branch back to header or to exit based on condition.
    ctx_.emitCondBr(continueCondition, headerBlock_, exitBlock_);

    // Patch PHI nodes with back-edge values.
    for (size_t i = 0; i < phiOps_.size(); ++i) {
        ctx_.patchPhiValue(phiOps_[i], 1, updatedVars[i]);
        ctx_.patchPhiBlock(phiOps_[i], 1, bodyEndBlock);
    }

    // Continue emitting into the exit block.
    ctx_.setInsertPoint(exitBlock_);
}

}  // namespace umbra::codegen
