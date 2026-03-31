#pragma once

#include <atomic>
#include <memory>

#include "ir/program.h"

#ifdef UMBRA_LLVM_BACKEND
#include "backend/llvm/llvm_compiler.h"
#endif

namespace umbra::execution {

/// AsyncCompiler — background LLVM compilation for adaptive execution.
///
/// Submits LLVM compilation to the global CompileThreadPool while Flying Start
/// executes morsels. The execution thread polls isReady() at morsel boundaries
/// (lock-free atomic check). When ready, getJitFunc() returns the LLVM-compiled
/// function pointer.
///
/// Lifecycle:
///   1. startAsync(program, funcId) — submits compilation to thread pool
///   2. isReady() — non-blocking poll at morsel boundaries
///   3. getJitFunc() — retrieve compiled function (only after isReady() == true)
///   4. ~AsyncCompiler() — non-blocking; pool worker owns SharedState via shared_ptr
///
/// Thread safety: SharedState is ref-counted (shared_ptr) so the pool worker
/// can outlive the AsyncCompiler. The ir::Program is shared read-only.
class AsyncCompiler {
   public:
    AsyncCompiler() = default;
    ~AsyncCompiler() = default;

    // Non-copyable, non-movable.
    AsyncCompiler(const AsyncCompiler&) = delete;
    AsyncCompiler& operator=(const AsyncCompiler&) = delete;
    AsyncCompiler(AsyncCompiler&&) = delete;
    AsyncCompiler& operator=(AsyncCompiler&&) = delete;

    /// Submit LLVM compilation to the thread pool.
    /// @param program Shared ownership of the IR program (kept alive for pool worker).
    /// @param funcId The function to compile.
    void startAsync(std::shared_ptr<ir::Program> program, ir::FunctionId funcId);

    /// Check if LLVM code is ready (non-blocking, lock-free).
    [[nodiscard]] bool isReady() const {
        return state_ && state_->ready.load(std::memory_order_acquire);
    }

    /// Get the compiled function pointer. Must only call after isReady() == true.
    [[nodiscard]] void* getJitFunc() const { return state_ ? state_->jitFunc : nullptr; }

    /// Get the LLVM compilation time in milliseconds.
    /// Returns 0 if compilation hasn't finished yet.
    [[nodiscard]] double compileMs() const { return state_ ? state_->compileMs : 0.0; }

    /// Whether the async compilation was started.
    [[nodiscard]] bool started() const { return started_; }

    /// Whether compilation failed (e.g., LLVM not available).
    [[nodiscard]] bool failed() const { return state_ && state_->failed; }

   private:
    /// Shared state accessed by both the pool worker and the AsyncCompiler.
    /// Using shared_ptr allows the pool worker to outlive the AsyncCompiler
    /// — when AsyncCompiler is destroyed, the worker keeps its reference.
    struct SharedState {
        std::atomic<bool> ready{false};
        bool failed{false};
        void* jitFunc{nullptr};
        double compileMs{0.0};
        std::shared_ptr<ir::Program> program;
#ifdef UMBRA_LLVM_BACKEND
        std::unique_ptr<llvm_backend::LLVMCompiler> compiler;
#endif
    };

    std::shared_ptr<SharedState> state_;
    bool started_{false};
};

}  // namespace umbra::execution
