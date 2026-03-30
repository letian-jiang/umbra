#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::llvm_backend {

/// LLVMCompiler — lowers Umbra IR to LLVM IR, optimizes with O3, JIT-compiles via ORC.
///
/// Usage mirrors FlyingStartCompiler:
///   LLVMCompiler compiler;
///   void* fn = compiler.compile(program, funcId);
///   LLVMCompiler::execute(fn, args);
///
/// The compiler instance must outlive any execution of compiled code (it owns the LLJIT).
class LLVMCompiler {
   public:
    LLVMCompiler();
    ~LLVMCompiler();

    // Non-copyable, non-movable (owns LLJIT).
    LLVMCompiler(const LLVMCompiler&) = delete;
    LLVMCompiler& operator=(const LLVMCompiler&) = delete;
    LLVMCompiler(LLVMCompiler&&) = delete;
    LLVMCompiler& operator=(LLVMCompiler&&) = delete;

    /// Compile a function. Returns the native function pointer.
    void* compile(ir::Program& program, ir::FunctionId func);

    /// Compile and capture optimized LLVM IR text.
    void* compileWithLog(ir::Program& program, ir::FunctionId func, std::string& llvmIrOutput);

    /// Execute a compiled function with arguments.
    static uint64_t execute(void* funcPtr, std::span<const uint64_t> args);

    /// Compile and execute in one step.
    uint64_t compileAndExecute(ir::Program& program, ir::FunctionId func,
                               std::span<const uint64_t> args);

   private:
    /// Opaque pointer to the LLJIT instance (avoids LLVM headers in this header).
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace umbra::llvm_backend
