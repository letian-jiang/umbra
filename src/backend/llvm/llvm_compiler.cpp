#include "backend/llvm/llvm_compiler.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stack>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ir/block.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

// LLVM headers — included only in this TU.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcomment"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsX86.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#pragma GCC diagnostic pop

#include "backend/llvm/host_inline_bitcode.h"

namespace umbra::llvm_backend {

// ── Host function trampoline ────────────────────────────────────────────────

extern "C" uint64_t umbraCallHost(void* hostFuncOpaque, const uint64_t* args, uint8_t count) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    auto* hostFunc = reinterpret_cast<ir::HostFunction*>(hostFuncOpaque);
    return (*hostFunc)(args, count);
}

// ── LLVM one-time initialization ────────────────────────────────────────────

static std::once_flag llvmInitFlag;  // NOLINT(cert-err58-cpp)

static void initLLVM() {
    std::call_once(llvmInitFlag, [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
    });
}

// ── Impl: all LLVM state lives here ─────────────────────────────────────────

struct LLVMCompiler::Impl {
    std::unique_ptr<llvm::orc::LLJIT> jit;

    // Per-compilation state (reset on each compile call).
    llvm::LLVMContext* ctx = nullptr;
    llvm::Module* mod = nullptr;
    llvm::IRBuilder<>* bld = nullptr;
    llvm::Function* func = nullptr;
    ir::Program* program = nullptr;

    std::unordered_map<uint32_t, llvm::BasicBlock*> blockMap;
    std::unordered_map<ir::IROp, llvm::Value*> valueMap;
    std::vector<uint32_t> rpoOrder;

    // ── Lowering ────────────────────────────────────────────────────────

    std::unique_ptr<llvm::Module> lowerToLLVM(ir::Program& prog, ir::FunctionId funcId,
                                              llvm::LLVMContext& llvmCtx);
    void discoverBlocks(ir::Program& prog, uint32_t entryBlockOff);
    void createPhiNodes(ir::Program& prog, uint32_t blockOff);
    void translateBlock(ir::Program& prog, uint32_t blockOff);
    void translateInstruction(ir::Program& prog, const ir::InstructionView& inst, uint32_t instOff);
    void populatePhiIncomings(ir::Program& prog);

    // ── Inline host functions ───────────────────────────────────────────

    /// Link embedded bitcode module (for inlineable functions) into the JIT module.
    void linkInlineBitcode(llvm::Module& targetModule, llvm::LLVMContext& llvmCtx);

    /// Check if any host function in the program is marked inlineable.
    static bool hasInlineableFunctions(const ir::Program& prog);

    // ── Helpers ──────────────────────────────────────────────────────────

    llvm::Type* toLLVMType(ir::TypeId type);
    llvm::Value* getValue(ir::IROp op);
    void setValue(ir::IROp op, llvm::Value* val);

    /// Coerce an LLVM value to the expected type (int↔ptr, width changes).
    llvm::Value* coerceArg(llvm::Value* val, llvm::Type* expectedTy);
};

// ── Constructor / Destructor ────────────────────────────────────────────────

LLVMCompiler::LLVMCompiler() : impl_(std::make_unique<Impl>()) {
    initLLVM();
}

LLVMCompiler::~LLVMCompiler() = default;

// ── execute (static) ────────────────────────────────────────────────────────

uint64_t LLVMCompiler::execute(void* funcPtr, std::span<const uint64_t> args) {
    switch (args.size()) {
        case 0: {
            using FnPtr = uint64_t (*)();
            return reinterpret_cast<FnPtr>(funcPtr)();  // NOLINT
        }
        case 1: {
            using FnPtr = uint64_t (*)(uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0]);  // NOLINT
        }
        case 2: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1]);  // NOLINT
        }
        case 3: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2]);  // NOLINT
        }
        case 4: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2], args[3]);  // NOLINT
        }
        case 5: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2], args[3],  // NOLINT
                                                    args[4]);
        }
        case 6: {
            using FnPtr = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
            return reinterpret_cast<FnPtr>(funcPtr)(args[0], args[1], args[2],  // NOLINT
                                                    args[3], args[4], args[5]);
        }
        default:
            assert(false && "Too many arguments");
            return 0;
    }
}

uint64_t LLVMCompiler::compileAndExecute(ir::Program& program, ir::FunctionId func,
                                         std::span<const uint64_t> args) {
    void* ptr = compile(program, func);
    return execute(ptr, args);
}

// ── compile ─────────────────────────────────────────────────────────────────

void* LLVMCompiler::compile(ir::Program& program, ir::FunctionId func) {
    std::string dummy;
    return compileWithLog(program, func, dummy);
}

void* LLVMCompiler::compileWithLog(ir::Program& program, ir::FunctionId func,
                                   std::string& llvmIrOutput) {
    // 1. Create LLVM context + module.
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();

    // 2. Lower Umbra IR → LLVM IR.
    auto mod = impl_->lowerToLLVM(program, func, *llvmCtx);

    // 3. Link inline bitcode for functions marked as inlineable.
    if (Impl::hasInlineableFunctions(program)) {
        impl_->linkInlineBitcode(*mod, *llvmCtx);
    }

    // 4. Verify the module.
    std::string verifyErr;
    llvm::raw_string_ostream verifyStream(verifyErr);
    if (llvm::verifyModule(*mod, &verifyStream)) {
        verifyStream.flush();
        throw std::runtime_error("LLVM IR verification failed: " + verifyErr);
    }

    // 5. Optimize with O3 (the inliner will inline alwaysinline functions).
    {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;

        llvm::PassBuilder pb;
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);

        // O3: backend validated via three-backend cross-validation on TPC-H Q1.
        auto mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
        mpm.run(*mod, mam);
    }

    // 5b. Capture IR text (after optimization).
    {
        llvm::raw_string_ostream irStream(llvmIrOutput);
        mod->print(irStream, nullptr);
        irStream.flush();
    }

    // Debug: also dump to stderr if env is set.
    if (std::getenv("UMBRA_LLVM_DUMP") != nullptr) {
        mod->dump();
    }

    // 6. Create LLJIT and compile.
    auto jitExpected = llvm::orc::LLJITBuilder().create();
    if (!jitExpected) {
        throw std::runtime_error("Failed to create LLJIT");
    }
    impl_->jit = std::move(*jitExpected);

    // Register absolute symbols: trampoline + direct C ABI functions.
    auto& mainJD = impl_->jit->getMainJITDylib();
    llvm::orc::SymbolMap symbols;
    symbols[impl_->jit->mangleAndIntern("umbraCallHost")] =
        llvm::JITEvaluatedSymbol(llvm::pointerToJITTargetAddress(&umbraCallHost),
                                 llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);

    // Register direct host function symbols (for non-inlined direct calls).
    for (uint32_t fid = 0; fid < program.numFunctions(); ++fid) {
        const auto& meta = program.functionMeta(fid);
        if (meta.directPtr != nullptr && !meta.inlineable) {
            std::string sym = "umbra_" + meta.name;
            symbols[impl_->jit->mangleAndIntern(sym)] = llvm::JITEvaluatedSymbol(
                llvm::pointerToJITTargetAddress(meta.directPtr),
                llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
        }
    }

    if (auto err = mainJD.define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
        throw std::runtime_error("Failed to register absolute symbols");
    }

    // Get function name before module is moved.
    const auto& meta = program.functionMeta(func);
    std::string funcName = meta.name;

    auto tsm = llvm::orc::ThreadSafeModule(std::move(mod), std::move(llvmCtx));
    if (auto err = impl_->jit->addIRModule(std::move(tsm))) {
        throw std::runtime_error("Failed to add IR module to LLJIT");
    }

    // 6. Look up the compiled function.
    auto symExpected = impl_->jit->lookup(funcName);
    if (!symExpected) {
        throw std::runtime_error("Failed to look up function: " + funcName);
    }
    return reinterpret_cast<void*>(symExpected->getAddress());  // NOLINT
}

// ── lowerToLLVM ─────────────────────────────────────────────────────────────

std::unique_ptr<llvm::Module> LLVMCompiler::Impl::lowerToLLVM(ir::Program& prog,
                                                              ir::FunctionId funcId,
                                                              llvm::LLVMContext& llvmCtx) {
    blockMap.clear();
    valueMap.clear();
    rpoOrder.clear();
    ctx = &llvmCtx;
    program = &prog;

    auto module = std::make_unique<llvm::Module>("umbra_module", llvmCtx);
    mod = module.get();

    const auto& meta = prog.functionMeta(funcId);

    // Function type: all params and return as i64 (ABI convention).
    // Even void functions return i64 to match the execute() trampoline.
    llvm::Type* retTy = llvm::Type::getInt64Ty(llvmCtx);
    std::vector<llvm::Type*> paramTypes(meta.numParams, llvm::Type::getInt64Ty(llvmCtx));
    auto* fnTy = llvm::FunctionType::get(retTy, paramTypes, false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::ExternalLinkage, meta.name, *module);
    func = fn;

    llvm::IRBuilder<> irBuilder(llvmCtx);
    bld = &irBuilder;

    // Discover blocks and compute RPO.
    uint32_t entryBlockOff = prog.functionEntry(funcId).entryBlockOffset;
    discoverBlocks(prog, entryBlockOff);

    // Create LLVM basic blocks.
    for (uint32_t blockOff : rpoOrder) {
        auto* bb = llvm::BasicBlock::Create(llvmCtx, "bb_" + std::to_string(blockOff), fn);
        blockMap[blockOff] = bb;
    }

    // Phase 1: Create empty PHI nodes.
    for (uint32_t blockOff : rpoOrder) {
        createPhiNodes(prog, blockOff);
    }

    // Phase 2: Translate non-PHI instructions in RPO order.
    for (uint32_t blockOff : rpoOrder) {
        translateBlock(prog, blockOff);
    }

    // Phase 3: Populate PHI incoming values.
    populatePhiIncomings(prog);

    // Cleanup.
    bld = nullptr;
    func = nullptr;
    ctx = nullptr;
    mod = nullptr;
    program = nullptr;

    return module;
}

// ── Block discovery (DFS post-order → reverse = RPO) ────────────────────────

void LLVMCompiler::Impl::discoverBlocks(ir::Program& prog, uint32_t entryBlockOff) {
    rpoOrder.clear();
    std::unordered_set<uint32_t> visited;
    std::vector<uint32_t> postOrder;

    struct Frame {
        uint32_t blockOff;
        bool childrenPushed;
    };
    std::stack<Frame> stk;
    stk.push({entryBlockOff, false});
    visited.insert(entryBlockOff);

    while (!stk.empty()) {
        auto& frame = stk.top();
        if (!frame.childrenPushed) {
            frame.childrenPushed = true;

            auto blockView = prog.blockAt(frame.blockOff);
            ir::InstructionView terminator{nullptr, 0};
            for (auto instView : blockView) {
                terminator = instView;
            }

            auto opcode = terminator.opcode();
            if (opcode == ir::Opcode::BR) {
                uint32_t target = terminator.arg(0);
                if (visited.insert(target).second) {
                    stk.push({target, false});
                }
            } else if (opcode == ir::Opcode::COND_BR) {
                uint32_t trueBB = terminator.arg(1);
                uint32_t falseBB = terminator.arg(2);
                if (visited.insert(falseBB).second) {
                    stk.push({falseBB, false});
                }
                if (visited.insert(trueBB).second) {
                    stk.push({trueBB, false});
                }
            } else if (opcode == ir::Opcode::CHECKED_ADD || opcode == ir::Opcode::CHECKED_SUB ||
                       opcode == ir::Opcode::CHECKED_MUL) {
                uint32_t contBB = terminator.arg(2);
                uint32_t ovfBB = terminator.arg(3);
                if (visited.insert(ovfBB).second) {
                    stk.push({ovfBB, false});
                }
                if (visited.insert(contBB).second) {
                    stk.push({contBB, false});
                }
            }
        } else {
            postOrder.push_back(frame.blockOff);
            stk.pop();
        }
    }

    rpoOrder.assign(postOrder.rbegin(), postOrder.rend());
}

// ── Phase 1: Create PHI nodes ───────────────────────────────────────────────

void LLVMCompiler::Impl::createPhiNodes(ir::Program& prog, uint32_t blockOff) {
    bld->SetInsertPoint(blockMap[blockOff]);

    auto blockView = prog.blockAt(blockOff);
    for (auto instView : blockView) {
        if (!ir::isPhi(instView.opcode())) {
            break;
        }
        uint8_t numIncoming = ir::phiIncomingCount(instView.opcode());
        llvm::Type* phiType = toLLVMType(instView.type());
        auto* phi = bld->CreatePHI(phiType, numIncoming);
        setValue(instView.offset(), phi);
    }
}

// ── Phase 2: Translate blocks ───────────────────────────────────────────────

void LLVMCompiler::Impl::translateBlock(ir::Program& prog, uint32_t blockOff) {
    bld->SetInsertPoint(blockMap[blockOff]);

    auto blockView = prog.blockAt(blockOff);
    for (auto instView : blockView) {
        if (ir::isPhi(instView.opcode())) {
            continue;
        }
        translateInstruction(prog, instView, instView.offset());
    }
}

// ── Phase 3: Populate PHI incoming ──────────────────────────────────────────

void LLVMCompiler::Impl::populatePhiIncomings(ir::Program& prog) {
    for (uint32_t blockOff : rpoOrder) {
        auto blockView = prog.blockAt(blockOff);
        for (auto instView : blockView) {
            if (!ir::isPhi(instView.opcode())) {
                break;
            }
            auto* phi = llvm::cast<llvm::PHINode>(getValue(instView.offset()));
            uint8_t numIncoming = ir::phiIncomingCount(instView.opcode());
            for (uint8_t i = 0; i < numIncoming; ++i) {
                uint32_t srcBlockOff = instView.arg(i * 2);
                ir::IROp valOp = instView.arg(i * 2 + 1);
                llvm::Value* val = getValue(valOp);
                llvm::BasicBlock* srcBB = blockMap.at(srcBlockOff);

                if (val->getType() != phi->getType()) {
                    auto* term = srcBB->getTerminator();
                    assert(term && "Source block must have a terminator");
                    llvm::IRBuilder<> tmp(term);
                    if (val->getType()->isIntegerTy() && phi->getType()->isIntegerTy()) {
                        unsigned srcBits = val->getType()->getIntegerBitWidth();
                        unsigned dstBits = phi->getType()->getIntegerBitWidth();
                        if (srcBits < dstBits) {
                            val = tmp.CreateSExt(val, phi->getType());
                        } else {
                            val = tmp.CreateTrunc(val, phi->getType());
                        }
                    }
                }
                phi->addIncoming(val, srcBB);
            }
        }
    }
}

// ── Instruction translation ─────────────────────────────────────────────────

void LLVMCompiler::Impl::translateInstruction(ir::Program& prog, const ir::InstructionView& inst,
                                              uint32_t instOff) {
    using Op = ir::Opcode;
    auto opcode = inst.opcode();
    auto type = inst.type();
    auto* i8Ty = llvm::Type::getInt8Ty(*ctx);
    auto* i32Ty = llvm::Type::getInt32Ty(*ctx);
    auto* i64Ty = llvm::Type::getInt64Ty(*ctx);
    auto* i8PtrTy = llvm::Type::getInt8PtrTy(*ctx);

    switch (opcode) {
        // ── Constants & Arguments ───────────────────────────────────────
        case Op::CONST: {
            uint64_t payload = inst.constPayload();
            if (type == ir::TypeId::PTR) {
                setValue(instOff,
                         bld->CreateIntToPtr(llvm::ConstantInt::get(i64Ty, payload), i8PtrTy));
            } else {
                setValue(instOff,
                         llvm::ConstantInt::get(toLLVMType(type), payload, /*isSigned=*/true));
            }
            break;
        }
        case Op::FUNARG: {
            uint32_t index = inst.arg(0);
            llvm::Value* arg = func->getArg(index);
            llvm::Type* targetTy = toLLVMType(type);
            if (targetTy != arg->getType()) {
                if (targetTy->isPointerTy()) {
                    arg = bld->CreateIntToPtr(arg, targetTy);
                } else {
                    arg = bld->CreateTrunc(arg, targetTy);
                }
            }
            setValue(instOff, arg);
            break;
        }

        // ── Arithmetic ──────────────────────────────────────────────────
        case Op::ADD:
            setValue(instOff, bld->CreateAdd(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::SUB:
            setValue(instOff, bld->CreateSub(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::MUL:
            setValue(instOff, bld->CreateMul(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::DIV:
            setValue(instOff, bld->CreateSDiv(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::UDIV:
            setValue(instOff, bld->CreateUDiv(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::MOD:
            setValue(instOff, bld->CreateSRem(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::NEG:
            setValue(instOff, bld->CreateNeg(getValue(inst.arg(1))));
            break;

        // ── Checked Arithmetic (terminators) ────────────────────────────
        case Op::CHECKED_ADD:
        case Op::CHECKED_SUB:
        case Op::CHECKED_MUL: {
            llvm::Value* lhs = getValue(inst.arg(0));
            llvm::Value* rhs = getValue(inst.arg(1));
            uint32_t contBlockOff = inst.arg(2);
            uint32_t ovfBlockOff = inst.arg(3);

            llvm::Intrinsic::ID iid =
                (opcode == Op::CHECKED_ADD)   ? llvm::Intrinsic::sadd_with_overflow
                : (opcode == Op::CHECKED_SUB) ? llvm::Intrinsic::ssub_with_overflow
                                              : llvm::Intrinsic::smul_with_overflow;

            llvm::Function* intr = llvm::Intrinsic::getDeclaration(mod, iid, {lhs->getType()});
            llvm::Value* result = bld->CreateCall(intr, {lhs, rhs});
            setValue(instOff, bld->CreateExtractValue(result, 0));
            llvm::Value* ovf = bld->CreateExtractValue(result, 1);
            bld->CreateCondBr(ovf, blockMap.at(ovfBlockOff), blockMap.at(contBlockOff));
            break;
        }

        // ── Bitwise ─────────────────────────────────────────────────────
        case Op::AND:
            setValue(instOff, bld->CreateAnd(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::OR:
            setValue(instOff, bld->CreateOr(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::XOR:
            setValue(instOff, bld->CreateXor(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::SHL:
            setValue(instOff, bld->CreateShl(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::ASHR:
            setValue(instOff, bld->CreateAShr(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::LSHR:
            setValue(instOff, bld->CreateLShr(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::ROTL: {
            llvm::Value* val = getValue(inst.arg(0));
            llvm::Value* shamt = getValue(inst.arg(1));
            auto* fshl =
                llvm::Intrinsic::getDeclaration(mod, llvm::Intrinsic::fshl, {val->getType()});
            setValue(instOff, bld->CreateCall(fshl, {val, val, shamt}));
            break;
        }
        case Op::ROTR: {
            llvm::Value* val = getValue(inst.arg(0));
            llvm::Value* shamt = getValue(inst.arg(1));
            auto* fshr =
                llvm::Intrinsic::getDeclaration(mod, llvm::Intrinsic::fshr, {val->getType()});
            setValue(instOff, bld->CreateCall(fshr, {val, val, shamt}));
            break;
        }

        // ── Comparison (→ BOOL as i8) ───────────────────────────────────
        case Op::CMP_EQ:
        case Op::CMP_NE:
        case Op::CMP_LT:
        case Op::CMP_LE:
        case Op::CMP_GT:
        case Op::CMP_GE: {
            llvm::Value* lhs = getValue(inst.arg(0));
            llvm::Value* rhs = getValue(inst.arg(1));

            ir::InstructionView lhsInst = prog.instructionAt(inst.arg(0));
            bool isUnsigned = (lhsInst.type() == ir::TypeId::UINT64);

            llvm::CmpInst::Predicate pred;
            switch (opcode) {
                case Op::CMP_EQ:
                    pred = llvm::CmpInst::ICMP_EQ;
                    break;
                case Op::CMP_NE:
                    pred = llvm::CmpInst::ICMP_NE;
                    break;
                case Op::CMP_LT:
                    pred = isUnsigned ? llvm::CmpInst::ICMP_ULT : llvm::CmpInst::ICMP_SLT;
                    break;
                case Op::CMP_LE:
                    pred = isUnsigned ? llvm::CmpInst::ICMP_ULE : llvm::CmpInst::ICMP_SLE;
                    break;
                case Op::CMP_GT:
                    pred = isUnsigned ? llvm::CmpInst::ICMP_UGT : llvm::CmpInst::ICMP_SGT;
                    break;
                case Op::CMP_GE:
                    pred = isUnsigned ? llvm::CmpInst::ICMP_UGE : llvm::CmpInst::ICMP_SGE;
                    break;
                default:
                    __builtin_unreachable();
            }
            setValue(instOff, bld->CreateZExt(bld->CreateICmp(pred, lhs, rhs), i8Ty));
            break;
        }

        // ── Logical ─────────────────────────────────────────────────────
        case Op::LNOT: {
            llvm::Value* val = getValue(inst.arg(0));
            setValue(instOff,
                     bld->CreateZExt(
                         bld->CreateICmpEQ(val, llvm::ConstantInt::get(val->getType(), 0)), i8Ty));
            break;
        }
        case Op::LAND:
            setValue(instOff, bld->CreateAnd(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::LOR:
            setValue(instOff, bld->CreateOr(getValue(inst.arg(0)), getValue(inst.arg(1))));
            break;
        case Op::SELECT: {
            llvm::Value* cond = getValue(inst.arg(0));
            llvm::Value* condBool =
                bld->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));
            setValue(instOff,
                     bld->CreateSelect(condBool, getValue(inst.arg(1)), getValue(inst.arg(2))));
            break;
        }

        // ── Cast ────────────────────────────────────────────────────────
        case Op::SEXT:
            setValue(instOff, bld->CreateSExt(getValue(inst.arg(0)), toLLVMType(type)));
            break;
        case Op::ZEXT:
            setValue(instOff, bld->CreateZExt(getValue(inst.arg(0)), toLLVMType(type)));
            break;
        case Op::TRUNC:
            setValue(instOff, bld->CreateTrunc(getValue(inst.arg(0)), toLLVMType(type)));
            break;
        case Op::BITCAST: {
            llvm::Value* val = getValue(inst.arg(0));
            llvm::Type* dstTy = toLLVMType(type);
            if (val->getType()->isPointerTy() && dstTy->isIntegerTy()) {
                setValue(instOff, bld->CreatePtrToInt(val, dstTy));
            } else if (val->getType()->isIntegerTy() && dstTy->isPointerTy()) {
                setValue(instOff, bld->CreateIntToPtr(val, dstTy));
            } else {
                setValue(instOff, bld->CreateBitCast(val, dstTy));
            }
            break;
        }
        case Op::PTR2INT:
            setValue(instOff, bld->CreatePtrToInt(getValue(inst.arg(0)), toLLVMType(type)));
            break;
        case Op::INT2PTR:
            setValue(instOff, bld->CreateIntToPtr(getValue(inst.arg(0)), i8PtrTy));
            break;

        // ── Memory ──────────────────────────────────────────────────────
        case Op::LOAD: {
            llvm::Value* ptr = getValue(inst.arg(0));
            llvm::Type* elemTy = toLLVMType(type);
            auto* ptrTy = llvm::PointerType::get(elemTy, 0);
            setValue(instOff, bld->CreateLoad(elemTy, bld->CreateBitCast(ptr, ptrTy)));
            break;
        }
        case Op::STORE: {
            llvm::Value* val = getValue(inst.arg(0));
            llvm::Value* ptr = getValue(inst.arg(1));
            auto* ptrTy = llvm::PointerType::get(val->getType(), 0);
            bld->CreateStore(val, bld->CreateBitCast(ptr, ptrTy));
            break;
        }
        case Op::GEP: {
            llvm::Value* base = getValue(inst.arg(0));
            llvm::Value* index = getValue(inst.arg(1));
            llvm::Type* elemTy = toLLVMType(type);
            auto* ptrTy = llvm::PointerType::get(elemTy, 0);
            llvm::Value* typedBase = bld->CreateBitCast(base, ptrTy);
            setValue(instOff,
                     bld->CreateBitCast(bld->CreateGEP(elemTy, typedBase, index), i8PtrTy));
            break;
        }

        // ── Control Flow ────────────────────────────────────────────────
        case Op::BR:
            bld->CreateBr(blockMap.at(inst.arg(0)));
            break;
        case Op::COND_BR: {
            llvm::Value* cond = getValue(inst.arg(0));
            auto* condBool = bld->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));
            bld->CreateCondBr(condBool, blockMap.at(inst.arg(1)), blockMap.at(inst.arg(2)));
            break;
        }
        case Op::RET: {
            llvm::Value* retVal = getValue(inst.arg(0));
            llvm::Type* fnRetTy = func->getReturnType();
            if (retVal->getType() != fnRetTy) {
                if (retVal->getType()->isIntegerTy() && fnRetTy->isIntegerTy()) {
                    unsigned srcBits = retVal->getType()->getIntegerBitWidth();
                    unsigned dstBits = fnRetTy->getIntegerBitWidth();
                    if (srcBits < dstBits) {
                        retVal = bld->CreateSExt(retVal, fnRetTy);
                    } else if (srcBits > dstBits) {
                        retVal = bld->CreateTrunc(retVal, fnRetTy);
                    }
                } else if (retVal->getType()->isPointerTy() && fnRetTy->isIntegerTy()) {
                    retVal = bld->CreatePtrToInt(retVal, fnRetTy);
                }
            }
            bld->CreateRet(retVal);
            break;
        }
        case Op::RET_VOID:
            // Return i64 0 (function is always i64-returning for ABI compatibility).
            bld->CreateRet(llvm::ConstantInt::get(i64Ty, 0));
            break;

        // ── Calls (Host Functions) ──────────────────────────────────────
        case Op::CALL0:
        case Op::CALL1:
        case Op::CALL2:
        case Op::CALL3:
        case Op::CALL4:
        case Op::CALL5:
        case Op::CALL6: {
            ir::FunctionId calleeId = inst.arg(0);
            uint8_t numParams = ir::callParamCount(opcode);
            const auto& calleeMeta = prog.functionMeta(calleeId);

            llvm::Value* result = nullptr;

            if (calleeMeta.directPtr != nullptr) {
                // ── Direct C ABI call (Level 1 / Level 2) ──────────────
                // Build the LLVM function type from the actual parameter types.
                std::vector<llvm::Type*> paramTys;
                paramTys.reserve(calleeMeta.paramTypes.size());
                for (auto pty : calleeMeta.paramTypes) {
                    paramTys.push_back(toLLVMType(pty));
                }
                llvm::Type* retTy = (calleeMeta.returnType == ir::TypeId::VOID)
                                        ? llvm::Type::getVoidTy(*ctx)
                                        : toLLVMType(calleeMeta.returnType);
                auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);

                // Declare (or reuse) the function.  For inlineable functions the
                // definition comes from linked bitcode; for others, LLJIT resolves
                // the symbol from the absolute-symbol table.
                std::string sym = "umbra_" + calleeMeta.name;
                llvm::FunctionCallee callee = mod->getOrInsertFunction(sym, fnTy);

                // Prepare arguments with proper type coercion.
                std::vector<llvm::Value*> args;
                args.reserve(numParams);
                for (uint8_t i = 0; i < numParams; ++i) {
                    llvm::Value* argVal = getValue(inst.arg(1 + i));
                    args.push_back(coerceArg(argVal, paramTys[i]));
                }

                result = bld->CreateCall(callee, args);
            } else {
                // ── Legacy trampoline path ─────────────────────────────
                const ir::HostFunction* hostFunc = prog.hostFunction(calleeId);
                assert(hostFunc != nullptr && "Only host function calls supported");

                auto hostAddr =
                    static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hostFunc));  // NOLINT
                llvm::Value* hostPtr =
                    bld->CreateIntToPtr(llvm::ConstantInt::get(i64Ty, hostAddr), i8PtrTy);

                llvm::IRBuilder<> entryBuilder(&func->getEntryBlock(),
                                               func->getEntryBlock().getFirstInsertionPt());
                llvm::Value* argsAlloca = entryBuilder.CreateAlloca(
                    i64Ty, llvm::ConstantInt::get(i32Ty, std::max(uint8_t(1), numParams)));
                for (uint8_t i = 0; i < numParams; ++i) {
                    llvm::Value* argVal = getValue(inst.arg(1 + i));
                    if (argVal->getType() != i64Ty) {
                        if (argVal->getType()->isPointerTy()) {
                            argVal = bld->CreatePtrToInt(argVal, i64Ty);
                        } else if (argVal->getType()->isIntegerTy()) {
                            argVal = bld->CreateZExt(argVal, i64Ty);
                        }
                    }
                    bld->CreateStore(argVal, bld->CreateGEP(i64Ty, argsAlloca,
                                                            llvm::ConstantInt::get(i32Ty, i)));
                }

                auto* trampolineTy = llvm::FunctionType::get(
                    i64Ty, {i8PtrTy, llvm::PointerType::get(i64Ty, 0), i8Ty}, false);
                llvm::FunctionCallee trampoline =
                    mod->getOrInsertFunction("umbraCallHost", trampolineTy);

                result = bld->CreateCall(
                    trampoline, {hostPtr, argsAlloca, llvm::ConstantInt::get(i8Ty, numParams)});
            }

            // Convert the return value to the expected IR type.
            if (calleeMeta.returnType != ir::TypeId::VOID) {
                llvm::Type* expectedRetTy = toLLVMType(calleeMeta.returnType);
                if (result->getType() != expectedRetTy) {
                    if (expectedRetTy->isPointerTy()) {
                        result = bld->CreateIntToPtr(result, expectedRetTy);
                    } else if (expectedRetTy->isIntegerTy() && result->getType()->isIntegerTy()) {
                        result = bld->CreateTrunc(result, expectedRetTy);
                    } else if (expectedRetTy->isIntegerTy() && result->getType()->isPointerTy()) {
                        result = bld->CreatePtrToInt(result, expectedRetTy);
                    }
                }
                setValue(instOff, result);
            }
            break;
        }

        // ── DB-Specific ─────────────────────────────────────────────────
        case Op::IS_NULL: {
            llvm::Value* val = getValue(inst.arg(0));
            llvm::Value* cmp;
            if (val->getType()->isPointerTy()) {
                cmp = bld->CreateICmpNE(bld->CreatePtrToInt(val, i64Ty),
                                        llvm::ConstantInt::get(i64Ty, 0));
            } else {
                cmp = bld->CreateICmpNE(val, llvm::ConstantInt::get(val->getType(), 0));
            }
            setValue(instOff, bld->CreateZExt(cmp, i8Ty));
            break;
        }
        case Op::CRC32: {
            llvm::Value* data = getValue(inst.arg(0));
            llvm::Value* seed = getValue(inst.arg(1));
            if (data->getType() != i64Ty) {
                data = bld->CreateZExt(data, i64Ty);
            }
            if (seed->getType() != i64Ty) {
                seed = bld->CreateZExt(seed, i64Ty);
            }
            auto* crc32Intr =
                llvm::Intrinsic::getDeclaration(mod, llvm::Intrinsic::x86_sse42_crc32_64_64);
            setValue(instOff, bld->CreateCall(crc32Intr, {seed, data}));
            break;
        }
        case Op::BSWAP: {
            llvm::Value* val = getValue(inst.arg(0));
            auto* bswapIntr =
                llvm::Intrinsic::getDeclaration(mod, llvm::Intrinsic::bswap, {val->getType()});
            setValue(instOff, bld->CreateCall(bswapIntr, {val}));
            break;
        }

        // ── NOP / PHI / SWITCH ──────────────────────────────────────────
        case Op::NOP:
            break;
        case Op::SWITCH:
            assert(false && "SWITCH not implemented");
            break;
        case Op::PHI2:
        case Op::PHI3:
        case Op::PHI4:
        case Op::PHI5:
        case Op::PHI6:
            break;
        case Op::NUM_OPCODES:
            __builtin_unreachable();
    }
}

// ── Helpers ─────────────────────────────────────────────────────────────────

llvm::Type* LLVMCompiler::Impl::toLLVMType(ir::TypeId type) {
    switch (type) {
        case ir::TypeId::VOID:
            return llvm::Type::getVoidTy(*ctx);
        case ir::TypeId::BOOL:
        case ir::TypeId::INT8:
            return llvm::Type::getInt8Ty(*ctx);
        case ir::TypeId::INT32:
            return llvm::Type::getInt32Ty(*ctx);
        case ir::TypeId::INT64:
        case ir::TypeId::UINT64:
            return llvm::Type::getInt64Ty(*ctx);
        case ir::TypeId::PTR:
            return llvm::Type::getInt8PtrTy(*ctx);
    }
    __builtin_unreachable();
}

llvm::Value* LLVMCompiler::Impl::getValue(ir::IROp op) {
    auto it = valueMap.find(op);
    if (it != valueMap.end()) {
        return it->second;
    }
    // On-demand materialization: CONSTs may be deduplicated and referenced
    // from blocks they don't appear in.
    auto inst = program->instructionAt(op);
    if (inst.opcode() == ir::Opcode::CONST) {
        uint64_t payload = inst.constPayload();
        llvm::Value* val = nullptr;
        if (inst.type() == ir::TypeId::PTR) {
            val = bld->CreateIntToPtr(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx), payload),
                                      llvm::Type::getInt8PtrTy(*ctx));
        } else {
            val = llvm::ConstantInt::get(toLLVMType(inst.type()), payload, /*isSigned=*/true);
        }
        setValue(op, val);
        return val;
    }
    assert(false && "IR value not found in valueMap and not a CONST");
    __builtin_unreachable();
}

void LLVMCompiler::Impl::setValue(ir::IROp op, llvm::Value* val) {
    valueMap[op] = val;
}

llvm::Value* LLVMCompiler::Impl::coerceArg(llvm::Value* val, llvm::Type* expectedTy) {
    if (val->getType() == expectedTy) {
        return val;
    }
    if (expectedTy->isPointerTy() && val->getType()->isIntegerTy()) {
        return bld->CreateIntToPtr(val, expectedTy);
    }
    if (expectedTy->isIntegerTy() && val->getType()->isPointerTy()) {
        return bld->CreatePtrToInt(val, expectedTy);
    }
    if (expectedTy->isIntegerTy() && val->getType()->isIntegerTy()) {
        unsigned srcBits = val->getType()->getIntegerBitWidth();
        unsigned dstBits = expectedTy->getIntegerBitWidth();
        if (srcBits < dstBits) {
            return bld->CreateZExt(val, expectedTy);
        }
        return bld->CreateTrunc(val, expectedTy);
    }
    return val;
}

// ── Inline host function support ───────────────────────────────────────────

bool LLVMCompiler::Impl::hasInlineableFunctions(const ir::Program& prog) {
    for (uint32_t i = 0; i < prog.numFunctions(); ++i) {
        if (prog.functionMeta(i).inlineable) {
            return true;
        }
    }
    return false;
}

void LLVMCompiler::Impl::linkInlineBitcode(llvm::Module& targetModule, llvm::LLVMContext& llvmCtx) {
    // Parse the embedded bitcode.
    auto bufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(kHostInlineBitcode),  // NOLINT
                        kHostInlineBitcodeSize),
        "host_inline", /*RequiresNullTerminator=*/false);

    auto modOrErr = llvm::parseBitcodeFile(bufOrErr->getMemBufferRef(), llvmCtx);
    if (!modOrErr) {
        // If bitcode parsing fails, fall back to external symbol resolution.
        llvm::consumeError(modOrErr.takeError());
        return;
    }
    auto inlineModule = std::move(*modOrErr);

    // Mark all defined functions as alwaysinline.
    for (auto& fn : *inlineModule) {
        if (!fn.isDeclaration()) {
            fn.addFnAttr(llvm::Attribute::AlwaysInline);
            fn.setLinkage(llvm::GlobalValue::LinkOnceODRLinkage);
        }
    }

    // Link into the target module.  LinkOnlyNeeded pulls in only functions
    // that the target module actually references (via declarations).
    if (llvm::Linker::linkModules(targetModule, std::move(inlineModule),
                                  llvm::Linker::Flags::LinkOnlyNeeded)) {
        // Linking failed — non-fatal, fall back to external symbol resolution.
        return;
    }
}

}  // namespace umbra::llvm_backend
