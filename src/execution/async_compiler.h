#pragma once

#include <atomic>
#include <future>
#include <memory>

#include "ir/program.h"

#ifdef UMBRA_LLVM_BACKEND
#include "backend/llvm/llvm_compiler.h"
#endif

namespace umbra::execution {

/// AsyncCompiler — background LLVM compilation for adaptive execution.
///
/// Starts LLVM compilation on a separate thread while Flying Start executes morsels.
/// The execution thread polls isReady() at morsel boundaries (lock-free atomic check).
/// When ready, getJitFunc() returns the LLVM-compiled function pointer.
///
/// Lifecycle:
///   1. startAsync(program, funcId) — launches background thread
///   2. isReady() — non-blocking poll at morsel boundaries
///   3. getJitFunc() — retrieve compiled function (only after isReady() == true)
///   4. ~AsyncCompiler() — waits for background thread if still running
///
/// Thread safety: the ir::Program is shared read-only between FS (already done)
/// and LLVM (background). No data race since both only read the Program.
class AsyncCompiler {
   public:
    AsyncCompiler() = default;
    ~AsyncCompiler();

    // Non-copyable, non-movable.
    AsyncCompiler(const AsyncCompiler&) = delete;
    AsyncCompiler& operator=(const AsyncCompiler&) = delete;
    AsyncCompiler(AsyncCompiler&&) = delete;
    AsyncCompiler& operator=(AsyncCompiler&&) = delete;

    /// Start LLVM compilation on a background thread.
    /// The Program must outlive this object (or be shared via shared_ptr).
    /// @param program Shared ownership of the IR program (kept alive for background thread).
    /// @param funcId The function to compile.
    void startAsync(std::shared_ptr<ir::Program> program, ir::FunctionId funcId);

    /// Check if LLVM code is ready (non-blocking, lock-free).
    [[nodiscard]] bool isReady() const { return ready_.load(std::memory_order_acquire); }

    /// Get the compiled function pointer. Must only call after isReady() == true.
    [[nodiscard]] void* getJitFunc() const { return jitFunc_; }

    /// Get the LLVM compilation time in milliseconds.
    /// Returns 0 if compilation hasn't finished yet.
    [[nodiscard]] double compileMs() const { return compileMs_; }

    /// Whether the async compilation was started.
    [[nodiscard]] bool started() const { return started_; }

    /// Whether compilation failed (e.g., LLVM not available).
    [[nodiscard]] bool failed() const { return failed_; }

   private:
    std::future<void> future_;
    std::atomic<bool> ready_{false};
    bool started_{false};
    bool failed_{false};
    void* jitFunc_{nullptr};
    double compileMs_{0.0};

    /// Keep the Program alive for the background thread.
    std::shared_ptr<ir::Program> program_;

#ifdef UMBRA_LLVM_BACKEND
    /// The LLVM compiler instance — must outlive execution since it owns JIT'd code.
    std::unique_ptr<llvm_backend::LLVMCompiler> compiler_;
#endif
};

}  // namespace umbra::execution
