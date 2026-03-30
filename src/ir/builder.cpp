#include "ir/builder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <queue>
#include <unordered_set>

namespace umbra::ir {

IRBuilder::IRBuilder() = default;

// ── Program structure ────────────────────────────────────────────────────────

FunctionId IRBuilder::createFunction(const std::string& name, TypeId returnType,
                                     const std::vector<TypeId>& paramTypes) {
    auto id = static_cast<FunctionId>(functions_.size());
    // Entry block will be set when setInsertPoint is first called for a block of this function.
    functions_.push_back({INVALID_BLOCK});
    FunctionMeta m;
    m.numParams = static_cast<uint32_t>(paramTypes.size());
    m.returnType = returnType;
    m.name = name;
    m.paramTypes = paramTypes;
    meta_.push_back(std::move(m));
    currentFunction_ = id;

    // Create the entry block automatically.
    auto entry = createBlock();
    setInsertPoint(entry);
    functions_[id].entryBlockOffset = blockOffsets_[entry];

    return id;
}

BlockId IRBuilder::createBlock() {
    auto id = nextBlockId_++;
    return id;
}

void IRBuilder::setInsertPoint(BlockId block) {
    // If this block hasn't been started yet, record its offset in the block buffer.
    if (blockOffsets_.find(block) == blockOffsets_.end()) {
        blockOffsets_[block] = static_cast<uint32_t>(blockBuffer_.size());
    }
    currentBlock_ = block;
}

BlockId IRBuilder::getCurrentBlock() const {
    return currentBlock_;
}

FunctionId IRBuilder::getCurrentFunction() const {
    return currentFunction_;
}

void IRBuilder::setCurrentFunction(FunctionId func) {
    currentFunction_ = func;
}

// ── Constants and arguments ──────────────────────────────────────────────────

IROp IRBuilder::emitConst(TypeId type, uint64_t value) {
    // Deduplication key: combine type and value.
    uint64_t key = (static_cast<uint64_t>(type) << 56) | (value & 0x00FFFFFFFFFFFFFF);
    // For 8-byte values, use a different strategy: hash type+value.
    // Simple approach: use full value for small types, truncated for large.
    if (type == TypeId::INT64 || type == TypeId::UINT64 || type == TypeId::PTR) {
        // Use a combined key that includes the type byte and full value.
        // To handle collisions, store in a separate map or check value matches.
        key = value ^ (static_cast<uint64_t>(type) << 56);
    }

    auto it = constDedup_.find(key);
    if (it != constDedup_.end()) {
        // Verify the found constant actually matches (type and value).
        InstructionView view(instBuffer_.data(), it->second);
        if (view.type() == type && view.constPayload() == value) {
            return it->second;
        }
    }

    auto op = encodeConst(instBuffer_, type, value);
    // Add to current block.
    blockBuffer_.push_back(op);
    constDedup_[key] = op;
    return op;
}

IROp IRBuilder::emitFunArg(TypeId type, uint32_t index) {
    std::array<IROp, 1> args = {index};
    return emitRaw(Opcode::FUNARG, type, args.data(), 1);
}

// ── Arithmetic ───────────────────────────────────────────────────────────────

IROp IRBuilder::emitAdd(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::ADD, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::ADD, type, lhs, rhs);
}

IROp IRBuilder::emitSub(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::SUB, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::SUB, type, lhs, rhs);
}

IROp IRBuilder::emitMul(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::MUL, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::MUL, type, lhs, rhs);
}

IROp IRBuilder::emitDiv(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::DIV, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::DIV, type, lhs, rhs);
}

IROp IRBuilder::emitUDiv(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::UDIV, type, lhs, rhs);
}

IROp IRBuilder::emitMod(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::MOD, type, lhs, rhs);
}

IROp IRBuilder::emitNeg(TypeId type, IROp operand) {
    // Encoded as binary with 0 as first arg.
    auto zero = emitConst(type, 0);
    return emitBinary(Opcode::NEG, type, zero, operand);
}

// ── Checked arithmetic ───────────────────────────────────────────────────────

IROp IRBuilder::emitCheckedAdd(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow) {
    std::array<IROp, 4> args = {lhs, rhs, cont, overflow};
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 2);
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 3);
    return emitRaw(Opcode::CHECKED_ADD, type, args.data(), 4);
}

IROp IRBuilder::emitCheckedSub(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow) {
    std::array<IROp, 4> args = {lhs, rhs, cont, overflow};
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 2);
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 3);
    return emitRaw(Opcode::CHECKED_SUB, type, args.data(), 4);
}

IROp IRBuilder::emitCheckedMul(TypeId type, IROp lhs, IROp rhs, BlockId cont, BlockId overflow) {
    std::array<IROp, 4> args = {lhs, rhs, cont, overflow};
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 2);
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 3);
    return emitRaw(Opcode::CHECKED_MUL, type, args.data(), 4);
}

// ── Bitwise ──────────────────────────────────────────────────────────────────

IROp IRBuilder::emitAnd(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::AND, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::AND, type, lhs, rhs);
}

IROp IRBuilder::emitOr(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldBinary(Opcode::OR, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::OR, type, lhs, rhs);
}

IROp IRBuilder::emitXor(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::XOR, type, lhs, rhs);
}

IROp IRBuilder::emitShl(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::SHL, type, lhs, rhs);
}

IROp IRBuilder::emitAShr(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::ASHR, type, lhs, rhs);
}

IROp IRBuilder::emitLShr(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::LSHR, type, lhs, rhs);
}

IROp IRBuilder::emitRotl(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::ROTL, type, lhs, rhs);
}

IROp IRBuilder::emitRotr(TypeId type, IROp lhs, IROp rhs) {
    return emitBinary(Opcode::ROTR, type, lhs, rhs);
}

// ── Comparison ───────────────────────────────────────────────────────────────

IROp IRBuilder::emitCmpEq(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_EQ, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_EQ, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitCmpNe(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_NE, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_NE, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitCmpLt(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_LT, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_LT, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitCmpLe(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_LE, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_LE, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitCmpGt(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_GT, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_GT, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitCmpGe(TypeId type, IROp lhs, IROp rhs) {
    IROp result = INVALID_OP;
    if (tryFoldComparison(Opcode::CMP_GE, type, lhs, rhs, result)) {
        return result;
    }
    return emitBinary(Opcode::CMP_GE, TypeId::BOOL, lhs, rhs);
}

// ── Logical ──────────────────────────────────────────────────────────────────

IROp IRBuilder::emitLNot(IROp operand) {
    // Constant fold: !const
    if (isConst(operand)) {
        return emitConst(TypeId::BOOL, getConstValue(operand) != 0 ? 0 : 1);
    }
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::LNOT, TypeId::BOOL, args.data(), 1);
}

IROp IRBuilder::emitLAnd(IROp lhs, IROp rhs) {
    if (isConst(lhs)) {
        return getConstValue(lhs) != 0 ? rhs : lhs;
    }
    if (isConst(rhs)) {
        return getConstValue(rhs) != 0 ? lhs : rhs;
    }
    return emitBinary(Opcode::LAND, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitLOr(IROp lhs, IROp rhs) {
    if (isConst(lhs)) {
        return getConstValue(lhs) != 0 ? lhs : rhs;
    }
    if (isConst(rhs)) {
        return getConstValue(rhs) != 0 ? rhs : lhs;
    }
    return emitBinary(Opcode::LOR, TypeId::BOOL, lhs, rhs);
}

IROp IRBuilder::emitSelect(TypeId type, IROp cond, IROp trueVal, IROp falseVal) {
    // Constant fold: select(true, a, b) → a; select(false, a, b) → b
    if (isConst(cond)) {
        return getConstValue(cond) != 0 ? trueVal : falseVal;
    }
    std::array<IROp, 3> args = {cond, trueVal, falseVal};
    return emitRaw(Opcode::SELECT, type, args.data(), 3);
}

// ── Cast ─────────────────────────────────────────────────────────────────────

IROp IRBuilder::emitSExt(TypeId targetType, IROp operand) {
    if (isConst(operand)) {
        return emitConst(targetType, getConstValue(operand));
    }
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::SEXT, targetType, args.data(), 1);
}

IROp IRBuilder::emitZExt(TypeId targetType, IROp operand) {
    if (isConst(operand)) {
        uint64_t value = getConstValue(operand);
        uint64_t mask = ~uint64_t{0};
        switch (getConstType(operand)) {
            case TypeId::BOOL:
            case TypeId::INT8:
                mask = 0xFF;
                break;
            case TypeId::INT32:
                mask = 0xFFFFFFFFULL;
                break;
            case TypeId::INT64:
            case TypeId::UINT64:
            case TypeId::PTR:
            case TypeId::VOID:
                mask = ~uint64_t{0};
                break;
        }
        return emitConst(targetType, value & mask);
    }
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::ZEXT, targetType, args.data(), 1);
}

IROp IRBuilder::emitTrunc(TypeId targetType, IROp operand) {
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::TRUNC, targetType, args.data(), 1);
}

IROp IRBuilder::emitBitcast(TypeId targetType, IROp operand) {
    ir::InstructionView operandInst(instBuffer_.data(), operand);
    if (operandInst.type() == targetType ||
        ir::typeSize(operandInst.type()) == ir::typeSize(targetType)) {
        return operand;
    }
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::BITCAST, targetType, args.data(), 1);
}

IROp IRBuilder::emitPtr2Int(TypeId targetType, IROp operand) {
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::PTR2INT, targetType, args.data(), 1);
}

IROp IRBuilder::emitInt2Ptr(IROp operand) {
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::INT2PTR, TypeId::PTR, args.data(), 1);
}

// ── Memory ───────────────────────────────────────────────────────────────────

IROp IRBuilder::emitLoad(TypeId type, IROp ptr) {
    std::array<IROp, 1> args = {ptr};
    return emitRaw(Opcode::LOAD, type, args.data(), 1);
}

void IRBuilder::emitStore(IROp val, IROp ptr) {
    std::array<IROp, 2> args = {val, ptr};
    emitRaw(Opcode::STORE, TypeId::VOID, args.data(), 2);
}

IROp IRBuilder::emitGEP(IROp base, TypeId elemType, IROp index) {
    std::array<IROp, 2> args = {base, index};
    return emitRaw(Opcode::GEP, elemType, args.data(), 2);
}

// ── Control flow ─────────────────────────────────────────────────────────────

void IRBuilder::emitBr(BlockId target) {
    std::array<IROp, 1> args = {target};
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 0);
    emitRaw(Opcode::BR, TypeId::VOID, args.data(), 1);
}

void IRBuilder::emitCondBr(IROp cond, BlockId trueBB, BlockId falseBB) {
    // Constant fold: condbr(true, bb1, bb2) → br bb1
    if (isConst(cond)) {
        if (getConstValue(cond) != 0) {
            emitBr(trueBB);
        } else {
            emitBr(falseBB);
        }
        return;
    }
    std::array<IROp, 3> args = {cond, trueBB, falseBB};
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 1);
    addPatch(static_cast<uint32_t>(instBuffer_.size()), 2);
    emitRaw(Opcode::COND_BR, TypeId::VOID, args.data(), 3);
}

IROp IRBuilder::emitRet(TypeId type, IROp val) {
    std::array<IROp, 1> args = {val};
    return emitRaw(Opcode::RET, type, args.data(), 1);
}

void IRBuilder::emitRetVoid() {
    emitRaw(Opcode::RET_VOID, TypeId::VOID, nullptr, 0);
}

// ── Call ─────────────────────────────────────────────────────────────────────

IROp IRBuilder::emitCall(TypeId retType, FunctionId callee, const std::vector<IROp>& args) {
    auto paramCount = static_cast<uint8_t>(args.size());
    auto opcode = callOpcode(paramCount);

    std::vector<IROp> allArgs;
    allArgs.reserve(1 + args.size());
    allArgs.push_back(callee);
    allArgs.insert(allArgs.end(), args.begin(), args.end());

    return emitRaw(opcode, retType, allArgs.data(), static_cast<uint8_t>(allArgs.size()));
}

// ── PHI ──────────────────────────────────────────────────────────────────────

IROp IRBuilder::emitPhi(TypeId type, const std::vector<std::pair<BlockId, IROp>>& incoming) {
    auto count = static_cast<uint8_t>(incoming.size());
    auto opcode = phiOpcode(count);

    std::vector<IROp> args;
    args.reserve(static_cast<size_t>(count) * 2);
    auto instOff = static_cast<uint32_t>(instBuffer_.size());
    for (uint8_t i = 0; i < count; ++i) {
        args.push_back(incoming[i].first);   // block ID
        args.push_back(incoming[i].second);  // value
        // Record patch for block ID at even indices.
        addPatch(instOff, static_cast<uint8_t>(i * 2));
    }

    return emitRaw(opcode, type, args.data(), static_cast<uint8_t>(args.size()));
}

void IRBuilder::patchPhiValue(IROp phiOp, uint8_t incomingIndex, IROp newValue) {
    // PHI args are: [block0, val0, block1, val1, ...]
    // Value is at odd indices: 2*i + 1
    patchArg(instBuffer_, phiOp, static_cast<uint8_t>(incomingIndex * 2 + 1), newValue);
}

void IRBuilder::patchPhiBlock(IROp phiOp, uint8_t incomingIndex, BlockId newBlock) {
    patchArg(instBuffer_, phiOp, static_cast<uint8_t>(incomingIndex * 2), newBlock);
}

// ── DB-specific ──────────────────────────────────────────────────────────────

IROp IRBuilder::emitIsNull(IROp operand) {
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::IS_NULL, TypeId::BOOL, args.data(), 1);
}

IROp IRBuilder::emitCRC32(IROp data, IROp seed) {
    return emitBinary(Opcode::CRC32, TypeId::UINT64, data, seed);
}

IROp IRBuilder::emitBSwap(TypeId type, IROp operand) {
    std::array<IROp, 1> args = {operand};
    return emitRaw(Opcode::BSWAP, type, args.data(), 1);
}

// ── Finalization ─────────────────────────────────────────────────────────────

Program IRBuilder::finalize() {
    // Backpatch all block IDs to byte offsets.
    for (const auto& patch : patchList_) {
        uint32_t pos = patch.instOffset + 2 + 4 * static_cast<uint32_t>(patch.argIndex);
        uint32_t blockId = 0;
        std::memcpy(&blockId, instBuffer_.data() + pos, sizeof(uint32_t));

        auto it = blockOffsets_.find(blockId);
        if (it != blockOffsets_.end()) {
            uint32_t actualOffset = it->second;
            std::memcpy(instBuffer_.data() + pos, &actualOffset, sizeof(uint32_t));
        }
        // If not found, leave as-is (may be a forward reference that was never resolved).
    }

    Program prog;
    prog.instBuffer_ = std::move(instBuffer_);
    prog.blockBuffer_ = std::move(blockBuffer_);
    prog.functions_ = std::move(functions_);
    prog.meta_ = std::move(meta_);
    return prog;
}

void IRBuilder::eliminateDeadCode(Program& program) {
    auto& instBuf = program.instBuffer_;
    auto& blockBuf = program.blockBuffer_;
    const auto& functions = program.functions_;

    if (instBuf.empty()) {
        return;
    }

    // Step 1: Find all reachable blocks via BFS from each function's entry block.
    std::unordered_set<uint32_t> reachableBlockOffsets;
    for (const auto& func : functions) {
        if (func.entryBlockOffset == INVALID_BLOCK) {
            continue;  // Host function, no blocks.
        }
        std::queue<uint32_t> worklist;
        worklist.push(func.entryBlockOffset);
        while (!worklist.empty()) {
            uint32_t blockOff = worklist.front();
            worklist.pop();
            if (reachableBlockOffsets.count(blockOff) != 0) {
                continue;
            }
            reachableBlockOffsets.insert(blockOff);

            // Walk instructions in this block to find terminators.
            uint32_t pos = blockOff;
            while (pos < static_cast<uint32_t>(blockBuf.size())) {
                uint32_t instOff = blockBuf[pos];
                auto opcode = static_cast<Opcode>(instBuf[instOff]);
                ++pos;

                if (isTerminator(opcode)) {
                    // Extract branch targets.
                    if (opcode == Opcode::BR) {
                        uint32_t target = 0;
                        std::memcpy(&target, instBuf.data() + instOff + 2, 4);
                        worklist.push(target);
                    } else if (opcode == Opcode::COND_BR) {
                        uint32_t trueTarget = 0;
                        uint32_t falseTarget = 0;
                        std::memcpy(&trueTarget, instBuf.data() + instOff + 2 + 4, 4);
                        std::memcpy(&falseTarget, instBuf.data() + instOff + 2 + 8, 4);
                        worklist.push(trueTarget);
                        worklist.push(falseTarget);
                    } else if (opcode == Opcode::CHECKED_ADD || opcode == Opcode::CHECKED_SUB ||
                               opcode == Opcode::CHECKED_MUL) {
                        uint32_t contTarget = 0;
                        uint32_t overflowTarget = 0;
                        std::memcpy(&contTarget, instBuf.data() + instOff + 2 + 8, 4);
                        std::memcpy(&overflowTarget, instBuf.data() + instOff + 2 + 12, 4);
                        worklist.push(contTarget);
                        worklist.push(overflowTarget);
                    }
                    break;
                }
            }
        }
    }

    // Step 2: Collect all instruction offsets in reachable blocks.
    std::unordered_set<uint32_t> reachableInstOffsets;
    for (uint32_t blockOff : reachableBlockOffsets) {
        uint32_t pos = blockOff;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            reachableInstOffsets.insert(instOff);
            auto opcode = static_cast<Opcode>(instBuf[instOff]);
            ++pos;
            if (isTerminator(opcode)) {
                break;
            }
        }
    }

    // Step 3: Mark live instructions. Start with side-effecting instructions.
    std::unordered_set<uint32_t> liveInsts;
    std::queue<uint32_t> liveWorklist;

    for (uint32_t instOff : reachableInstOffsets) {
        auto opcode = static_cast<Opcode>(instBuf[instOff]);
        if (hasSideEffect(opcode) || isTerminator(opcode)) {
            liveInsts.insert(instOff);
            liveWorklist.push(instOff);
        }
    }

    // Step 4: Propagate liveness through operands.
    while (!liveWorklist.empty()) {
        uint32_t instOff = liveWorklist.front();
        liveWorklist.pop();

        InstructionView view(instBuf.data(), instOff);
        auto nargs = view.numArgs();
        for (uint8_t i = 0; i < nargs; ++i) {
            uint32_t operandOff = view.arg(i);
            // Only consider operand offsets that point into the instruction buffer
            // (not block offsets or function IDs or constant values).
            if (operandOff < instBuf.size() && reachableInstOffsets.count(operandOff) != 0 &&
                liveInsts.count(operandOff) == 0) {
                liveInsts.insert(operandOff);
                liveWorklist.push(operandOff);
            }
        }

        // For PHI nodes, also mark the incoming values as live.
        if (isPhi(view.opcode())) {
            auto count = phiIncomingCount(view.opcode());
            for (uint8_t j = 0; j < count; ++j) {
                uint32_t valOff = view.arg(j * 2 + 1);  // value at odd indices
                if (valOff < instBuf.size() && reachableInstOffsets.count(valOff) != 0 &&
                    liveInsts.count(valOff) == 0) {
                    liveInsts.insert(valOff);
                    liveWorklist.push(valOff);
                }
            }
        }
    }

    // Step 5: Collect ALL instruction offsets from ALL blocks (including unreachable).
    // We need this to NOP instructions in unreachable blocks.
    std::unordered_set<uint32_t> allInstOffsets;
    {
        // Scan through the block buffer. Each block is a sequence of instruction offsets
        // terminated by a terminator instruction.
        uint32_t pos = 0;
        while (pos < static_cast<uint32_t>(blockBuf.size())) {
            uint32_t instOff = blockBuf[pos];
            allInstOffsets.insert(instOff);
            if (instOff < instBuf.size()) {
                auto opcode = static_cast<Opcode>(instBuf[instOff]);
                ++pos;
                if (isTerminator(opcode)) {
                    // Skip to next block — but blocks are contiguous in the buffer,
                    // so just continue reading.
                    continue;
                }
            } else {
                ++pos;
            }
        }
    }

    // Step 6: Replace dead and unreachable instructions with NOP in-place.
    for (uint32_t instOff : allInstOffsets) {
        if (liveInsts.count(instOff) == 0) {
            instBuf[instOff] = static_cast<uint8_t>(Opcode::NOP);
        }
    }
}

// ── Private helpers ──────────────────────────────────────────────────────────

IROp IRBuilder::emitRaw(Opcode opcode, TypeId type, const IROp* args, uint8_t argCount) {
    auto op = encodeInstruction(instBuffer_, opcode, type, args, argCount);
    blockBuffer_.push_back(op);
    return op;
}

IROp IRBuilder::emitBinary(Opcode opcode, TypeId type, IROp lhs, IROp rhs) {
    std::array<IROp, 2> args = {lhs, rhs};
    return emitRaw(opcode, type, args.data(), 2);
}

bool IRBuilder::isConst(IROp op) const {
    if (op >= instBuffer_.size()) {
        return false;
    }
    return static_cast<Opcode>(instBuffer_[op]) == Opcode::CONST;
}

uint64_t IRBuilder::getConstValue(IROp op) const {
    InstructionView view(instBuffer_.data(), op);
    return view.constPayload();
}

TypeId IRBuilder::getConstType(IROp op) const {
    return static_cast<TypeId>(instBuffer_[op + 1]);
}

bool IRBuilder::tryFoldBinary(Opcode opcode, TypeId type, IROp lhs, IROp rhs, IROp& result) {
    bool lhsConst = isConst(lhs);
    bool rhsConst = isConst(rhs);

    if (lhsConst && rhsConst) {
        auto lval = static_cast<int64_t>(getConstValue(lhs));
        auto rval = static_cast<int64_t>(getConstValue(rhs));
        int64_t res = 0;
        switch (opcode) {
            case Opcode::ADD:
                res = lval + rval;
                break;
            case Opcode::SUB:
                res = lval - rval;
                break;
            case Opcode::MUL:
                res = lval * rval;
                break;
            case Opcode::DIV:
                if (rval == 0) {
                    return false;  // Don't fold div by zero.
                }
                res = lval / rval;
                break;
            default:
                // Bitwise folding
                if (opcode == Opcode::AND) {
                    res = lval & rval;
                } else if (opcode == Opcode::OR) {
                    res = lval | rval;
                } else {
                    return false;
                }
        }
        result = emitConst(type, static_cast<uint64_t>(res));
        return true;
    }

    // Identity and absorbing element optimizations.
    if (rhsConst) {
        auto rval = getConstValue(rhs);
        switch (opcode) {
            case Opcode::ADD:
            case Opcode::SUB:
            case Opcode::OR:
                if (rval == 0) {
                    result = lhs;
                    return true;
                }
                break;
            case Opcode::MUL:
                if (rval == 1) {
                    result = lhs;
                    return true;
                }
                if (rval == 0) {
                    result = emitConst(type, 0);
                    return true;
                }
                break;
            case Opcode::AND:
                if (rval == 0) {
                    result = emitConst(type, 0);
                    return true;
                }
                break;
            default:
                break;
        }
    }

    if (lhsConst) {
        auto lval = getConstValue(lhs);
        switch (opcode) {
            case Opcode::ADD:
            case Opcode::OR:
                if (lval == 0) {
                    result = rhs;
                    return true;
                }
                break;
            case Opcode::MUL:
                if (lval == 1) {
                    result = rhs;
                    return true;
                }
                if (lval == 0) {
                    result = emitConst(type, 0);
                    return true;
                }
                break;
            case Opcode::AND:
                if (lval == 0) {
                    result = emitConst(type, 0);
                    return true;
                }
                break;
            default:
                break;
        }
    }

    return false;
}

bool IRBuilder::tryFoldComparison(Opcode opcode, TypeId type, IROp lhs, IROp rhs, IROp& result) {
    if (!isConst(lhs) || !isConst(rhs)) {
        return false;
    }

    // Signed comparison for INT types, unsigned for UINT64.
    bool isUnsigned = (type == TypeId::UINT64);
    auto lval = getConstValue(lhs);
    auto rval = getConstValue(rhs);

    bool res = false;
    if (isUnsigned) {
        switch (opcode) {
            case Opcode::CMP_EQ:
                res = lval == rval;
                break;
            case Opcode::CMP_NE:
                res = lval != rval;
                break;
            case Opcode::CMP_LT:
                res = lval < rval;
                break;
            case Opcode::CMP_LE:
                res = lval <= rval;
                break;
            case Opcode::CMP_GT:
                res = lval > rval;
                break;
            case Opcode::CMP_GE:
                res = lval >= rval;
                break;
            default:
                return false;
        }
    } else {
        auto sl = static_cast<int64_t>(lval);
        auto sr = static_cast<int64_t>(rval);
        switch (opcode) {
            case Opcode::CMP_EQ:
                res = sl == sr;
                break;
            case Opcode::CMP_NE:
                res = sl != sr;
                break;
            case Opcode::CMP_LT:
                res = sl < sr;
                break;
            case Opcode::CMP_LE:
                res = sl <= sr;
                break;
            case Opcode::CMP_GT:
                res = sl > sr;
                break;
            case Opcode::CMP_GE:
                res = sl >= sr;
                break;
            default:
                return false;
        }
    }
    result = emitConst(TypeId::BOOL, res ? 1 : 0);
    return true;
}

void IRBuilder::addPatch(uint32_t instOffset, uint8_t argIndex) {
    patchList_.push_back({instOffset, argIndex});
}

}  // namespace umbra::ir
