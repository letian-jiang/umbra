#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ir/builder.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::codegen {

// Forward declarations for typed value wrappers.
class Bool;
class Int8;
class Int32;
class Int64;
class UInt64;
template <typename T>
class Ptr;

/// CodegenContext — Layer 5 of Tidy Tuples.
/// Wraps IRBuilder to provide typed, safe IR emission.
class CodegenContext {
   public:
    CodegenContext();

    /// Access the underlying IRBuilder.
    [[nodiscard]] ir::IRBuilder& builder() { return builder_; }
    [[nodiscard]] const ir::IRBuilder& builder() const { return builder_; }

    // ── Function creation ───────────────────────────────────────────────

    /// Create a new function.
    ir::FunctionId createFunction(const std::string& name, ir::TypeId returnType,
                                  const std::vector<ir::TypeId>& paramTypes);

    // ── Block management ────────────────────────────────────────────────

    /// Create a new basic block.
    ir::BlockId createBlock();

    /// Set insertion point to a block.
    void setInsertPoint(ir::BlockId block);

    /// Get current block.
    [[nodiscard]] ir::BlockId getCurrentBlock() const;

    // ── Constant creation ───────────────────────────────────────────────

    /// Create a typed constant Bool.
    Bool constBool(bool value);

    /// Create a typed constant Int8.
    Int8 constInt8(int8_t value);

    /// Create a typed constant Int32.
    Int32 constInt32(int32_t value);

    /// Create a typed constant Int64.
    Int64 constInt64(int64_t value);

    /// Create a typed constant UInt64.
    UInt64 constUInt64(uint64_t value);

    /// Create a typed null pointer constant.
    template <typename T>
    Ptr<T> constNull();

    /// Create a pointer from an integer constant (for embedding runtime addresses).
    template <typename T>
    Ptr<T> constPtr(const void* addr);

    // ── Function arguments ──────────────────────────────────────────────

    /// Get a typed function argument.
    template <typename T>
    T getArg(uint32_t index);

    // ── Control flow ────────────────────────────────────────────────────

    /// Emit unconditional branch.
    void emitBr(ir::BlockId target);

    /// Emit conditional branch.
    void emitCondBr(Bool cond, ir::BlockId trueBB, ir::BlockId falseBB);

    /// Emit void return.
    void emitRetVoid();

    /// Emit typed return.
    template <typename T>
    void emitRet(T value);

    // ── Logical operations ──────────────────────────────────────────────

    /// Logical NOT.
    Bool lnot(Bool operand);

    /// Logical AND.
    Bool land(Bool lhs, Bool rhs);

    /// Logical OR.
    Bool lor(Bool lhs, Bool rhs);

    // ── Select (ternary) ────────────────────────────────────────────────

    /// Select between two values based on a condition.
    template <typename T>
    T select(Bool cond, T trueVal, T falseVal);

    // ── Cast ────────────────────────────────────────────────────────────

    /// Sign extend.
    template <typename Target, typename Source>
    Target sext(Source operand);

    /// Zero extend.
    template <typename Target, typename Source>
    Target zext(Source operand);

    /// Truncate.
    template <typename Target, typename Source>
    Target trunc(Source operand);

    // ── PHI ─────────────────────────────────────────────────────────────

    /// Emit a two-way PHI node.
    template <typename T>
    T phi2(ir::BlockId block0, T val0, ir::BlockId block1, T val1);

    /// Patch a PHI's incoming value.
    void patchPhiValue(ir::IROp phiOp, uint8_t incomingIndex, ir::IROp newValue);

    /// Patch a PHI's incoming block.
    void patchPhiBlock(ir::IROp phiOp, uint8_t incomingIndex, ir::BlockId newBlock);

    // ── Host function registration ──────────────────────────────────────

    /// Register a host function by name with a raw function pointer.
    ir::FunctionId registerHostFunction(const std::string& name, ir::HostFunction func,
                                        ir::TypeId returnType,
                                        const std::vector<ir::TypeId>& paramTypes);

    /// Emit a call instruction.
    template <typename Ret, typename... Args>
    Ret call(ir::FunctionId callee, Args... args);

    // ── Finalization ────────────────────────────────────────────────────

    /// Finalize the IR program.
    ir::Program finalize();

   private:
    ir::IRBuilder builder_;

    /// Deferred host function registrations (applied on finalize).
    struct HostFuncEntry {
        ir::FunctionId id;
        ir::HostFunction func;
    };
    std::vector<HostFuncEntry> hostFuncs_;
};

// ── Typed value wrappers ────────────────────────────────────────────────────

/// Trait to get the IR TypeId for a codegen type.
template <typename T>
struct TypeIdOf;

/// Base class for all typed value wrappers (CRTP).
/// Each wrapper holds a single IROp.
template <typename Derived>
class Value {
   public:
    Value() : op_(ir::INVALID_OP) {}
    // NOLINTNEXTLINE(google-explicit-constructor)
    Value(ir::IROp op) : op_(op) {}

    /// Get the underlying IROp.
    [[nodiscard]] ir::IROp op() const { return op_; }

    /// Check if the value is valid.
    [[nodiscard]] bool valid() const { return op_ != ir::INVALID_OP; }

    /// Get the IR TypeId.
    static ir::TypeId irType() { return TypeIdOf<Derived>::VALUE; }

   protected:
    ir::IROp op_;
};

/// Bool type — corresponds to IR BOOL.
class Bool : public Value<Bool> {
   public:
    using Value::Value;
};

template <>
struct TypeIdOf<Bool> {
    static constexpr ir::TypeId VALUE = ir::TypeId::BOOL;
};

/// Int8 type — corresponds to IR INT8.
class Int8 : public Value<Int8> {
   public:
    using Value::Value;
};

template <>
struct TypeIdOf<Int8> {
    static constexpr ir::TypeId VALUE = ir::TypeId::INT8;
};

/// Int32 type — corresponds to IR INT32.
class Int32 : public Value<Int32> {
   public:
    using Value::Value;
};

template <>
struct TypeIdOf<Int32> {
    static constexpr ir::TypeId VALUE = ir::TypeId::INT32;
};

/// Int64 type — corresponds to IR INT64.
class Int64 : public Value<Int64> {
   public:
    using Value::Value;
};

template <>
struct TypeIdOf<Int64> {
    static constexpr ir::TypeId VALUE = ir::TypeId::INT64;
};

/// UInt64 type — corresponds to IR UINT64.
class UInt64 : public Value<UInt64> {
   public:
    using Value::Value;
};

template <>
struct TypeIdOf<UInt64> {
    static constexpr ir::TypeId VALUE = ir::TypeId::UINT64;
};

/// Ptr<T> type — typed pointer.
/// T is the pointed-to type, used only at C++ compile time for type safety.
template <typename T>
class Ptr : public Value<Ptr<T>> {
   public:
    using Value<Ptr<T>>::Value;

    /// Load a value from this pointer.
    T load(CodegenContext& ctx) const {
        return T(ctx.builder().emitLoad(TypeIdOf<T>::VALUE, this->op_));
    }

    /// Store a value to this pointer.
    void store(CodegenContext& ctx, T val) const { ctx.builder().emitStore(val.op(), this->op_); }

    /// Get element pointer (array indexing).
    template <typename Idx>
    Ptr<T> gep(CodegenContext& ctx, Idx index) const {
        return Ptr<T>(ctx.builder().emitGEP(this->op_, TypeIdOf<T>::VALUE, index.op()));
    }

    /// Bitcast to a pointer to a different type.
    template <typename U>
    Ptr<U> bitcastTo(CodegenContext& ctx) const {
        return Ptr<U>(ctx.builder().emitBitcast(ir::TypeId::PTR, this->op_));
    }
};

/// TypeIdOf specializations for Ptr types.
template <typename T>
struct TypeIdOf<Ptr<T>> {
    static constexpr ir::TypeId VALUE = ir::TypeId::PTR;
};

// ── Operator overloads ──────────────────────────────────────────────────────

// We need a way to get the CodegenContext from values for operator overloads.
// Since the design says operators "emit IR instructions", we use a thread-local
// context pointer.

/// Set/get the active CodegenContext for operator overloads.
void setActiveContext(CodegenContext* ctx);
CodegenContext& activeContext();

/// Arithmetic operators for Int32.
Int32 operator+(Int32 lhs, Int32 rhs);
Int32 operator-(Int32 lhs, Int32 rhs);
Int32 operator*(Int32 lhs, Int32 rhs);
Int32 operator/(Int32 lhs, Int32 rhs);

/// Comparison operators for Int32.
Bool operator==(Int32 lhs, Int32 rhs);
Bool operator!=(Int32 lhs, Int32 rhs);
Bool operator<(Int32 lhs, Int32 rhs);
Bool operator<=(Int32 lhs, Int32 rhs);
Bool operator>(Int32 lhs, Int32 rhs);
Bool operator>=(Int32 lhs, Int32 rhs);

/// Arithmetic operators for Int64.
Int64 operator+(Int64 lhs, Int64 rhs);
Int64 operator-(Int64 lhs, Int64 rhs);
Int64 operator*(Int64 lhs, Int64 rhs);
Int64 operator/(Int64 lhs, Int64 rhs);

/// Comparison operators for Int64.
Bool operator==(Int64 lhs, Int64 rhs);
Bool operator!=(Int64 lhs, Int64 rhs);
Bool operator<(Int64 lhs, Int64 rhs);
Bool operator<=(Int64 lhs, Int64 rhs);
Bool operator>(Int64 lhs, Int64 rhs);
Bool operator>=(Int64 lhs, Int64 rhs);

/// Arithmetic operators for UInt64.
UInt64 operator+(UInt64 lhs, UInt64 rhs);
UInt64 operator-(UInt64 lhs, UInt64 rhs);
UInt64 operator*(UInt64 lhs, UInt64 rhs);
UInt64 operator/(UInt64 lhs, UInt64 rhs);

/// Comparison operators for UInt64.
Bool operator==(UInt64 lhs, UInt64 rhs);
Bool operator!=(UInt64 lhs, UInt64 rhs);
Bool operator<(UInt64 lhs, UInt64 rhs);
Bool operator<=(UInt64 lhs, UInt64 rhs);
Bool operator>(UInt64 lhs, UInt64 rhs);
Bool operator>=(UInt64 lhs, UInt64 rhs);

/// Bitwise XOR for UInt64.
UInt64 operator^(UInt64 lhs, UInt64 rhs);

// ── RAII Control Flow ───────────────────────────────────────────────────────

/// RAII If-then-else construct.
class IfBuilder {
   public:
    /// Construct an if-then with condition. Enters the then-block.
    IfBuilder(CodegenContext& ctx, Bool cond);

    // Prevent copy.
    IfBuilder(const IfBuilder&) = delete;
    IfBuilder& operator=(const IfBuilder&) = delete;
    IfBuilder(IfBuilder&&) = delete;
    IfBuilder& operator=(IfBuilder&&) = delete;

    /// Switch to the else block.
    void elseBlock();

    /// Finalize the if construct, switch to merge block.
    void done();

    /// Create a PHI node merging values from then and else blocks.
    template <typename T>
    T phi(T thenVal, T elseVal);

    /// Get the then-block ID.
    [[nodiscard]] ir::BlockId thenBlock() const { return thenBlock_; }

    /// Get the else-block ID.
    [[nodiscard]] ir::BlockId elseBlockId() const { return elseBlock_; }

    /// Get the merge-block ID.
    [[nodiscard]] ir::BlockId mergeBlock() const { return mergeBlock_; }

    ~IfBuilder() = default;

   private:
    CodegenContext& ctx_;
    ir::BlockId thenBlock_;
    ir::BlockId elseBlock_;
    ir::BlockId mergeBlock_;
    ir::BlockId thenEndBlock_;  // block at end of then-branch (after branches)
    ir::BlockId elseEndBlock_;  // block at end of else-branch
    bool hasElse_ = false;
};

/// RAII Loop construct with PHI loop variables.
class LoopBuilder {
   public:
    /// Construct a loop.
    /// loopVarInits: initial values for PHI loop variables.
    /// Entry condition is checked at the header.
    LoopBuilder(CodegenContext& ctx, const std::vector<ir::IROp>& loopVarInits,
                const std::vector<ir::TypeId>& loopVarTypes);

    // Prevent copy/move.
    LoopBuilder(const LoopBuilder&) = delete;
    LoopBuilder& operator=(const LoopBuilder&) = delete;
    LoopBuilder(LoopBuilder&&) = delete;
    LoopBuilder& operator=(LoopBuilder&&) = delete;

    /// Get a loop variable by index with the correct type.
    template <typename T>
    T getLoopVar(size_t index);

    /// Finish the loop body. Provide exit condition and updated loop variable values.
    /// If exitCond is true, loop continues; if false, exits.
    void done(Bool continueCondition, const std::vector<ir::IROp>& updatedVars);

    /// Get the exit block (valid after done()).
    [[nodiscard]] ir::BlockId exitBlock() const { return exitBlock_; }

    /// Get the header block.
    [[nodiscard]] ir::BlockId headerBlock() const { return headerBlock_; }

    ~LoopBuilder() = default;

   private:
    CodegenContext& ctx_;
    ir::BlockId preheaderBlock_;
    ir::BlockId headerBlock_;
    ir::BlockId bodyBlock_;
    ir::BlockId exitBlock_;
    std::vector<ir::IROp> phiOps_;  // PHI IROp for each loop variable
    std::vector<ir::TypeId> varTypes_;
};

// ── Template implementations ────────────────────────────────────────────────

template <typename T>
Ptr<T> CodegenContext::constNull() {
    return Ptr<T>(builder_.emitConst(ir::TypeId::PTR, 0));
}

template <typename T>
Ptr<T> CodegenContext::constPtr(const void* addr) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto val = reinterpret_cast<uintptr_t>(addr);
    return Ptr<T>(builder_.emitConst(ir::TypeId::PTR, val));
}

template <typename T>
T CodegenContext::getArg(uint32_t index) {
    return T(builder_.emitFunArg(TypeIdOf<T>::VALUE, index));
}

template <typename T>
void CodegenContext::emitRet(T value) {
    builder_.emitRet(TypeIdOf<T>::VALUE, value.op());
}

template <typename T>
T CodegenContext::select(Bool cond, T trueVal, T falseVal) {
    return T(builder_.emitSelect(TypeIdOf<T>::VALUE, cond.op(), trueVal.op(), falseVal.op()));
}

template <typename Target, typename Source>
Target CodegenContext::sext(Source operand) {
    return Target(builder_.emitSExt(TypeIdOf<Target>::VALUE, operand.op()));
}

template <typename Target, typename Source>
Target CodegenContext::zext(Source operand) {
    return Target(builder_.emitZExt(TypeIdOf<Target>::VALUE, operand.op()));
}

template <typename Target, typename Source>
Target CodegenContext::trunc(Source operand) {
    return Target(builder_.emitTrunc(TypeIdOf<Target>::VALUE, operand.op()));
}

template <typename T>
T CodegenContext::phi2(ir::BlockId block0, T val0, ir::BlockId block1, T val1) {
    return T(builder_.emitPhi(TypeIdOf<T>::VALUE, {{block0, val0.op()}, {block1, val1.op()}}));
}

// Helper to extract IROp from typed values for call.
namespace detail {

inline ir::IROp toOp(Bool v) {
    return v.op();
}
inline ir::IROp toOp(Int8 v) {
    return v.op();
}
inline ir::IROp toOp(Int32 v) {
    return v.op();
}
inline ir::IROp toOp(Int64 v) {
    return v.op();
}
inline ir::IROp toOp(UInt64 v) {
    return v.op();
}
template <typename T>
ir::IROp toOp(Ptr<T> v) {
    return v.op();
}

}  // namespace detail

template <typename Ret, typename... Args>
Ret CodegenContext::call(ir::FunctionId callee, Args... args) {
    std::vector<ir::IROp> argOps = {detail::toOp(args)...};
    return Ret(builder_.emitCall(TypeIdOf<Ret>::VALUE, callee, argOps));
}

template <typename T>
T IfBuilder::phi(T thenVal, T elseVal) {
    return T(ctx_.builder().emitPhi(
        TypeIdOf<T>::VALUE, {{thenEndBlock_, thenVal.op()}, {elseEndBlock_, elseVal.op()}}));
}

template <typename T>
T LoopBuilder::getLoopVar(size_t index) {
    return T(phiOps_[index]);
}

}  // namespace umbra::codegen
