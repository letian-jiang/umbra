#include "execution/async_compiler.h"

#include <chrono>

#include "execution/compile_thread_pool.h"

namespace umbra::execution {

void AsyncCompiler::startAsync(std::shared_ptr<ir::Program> program, ir::FunctionId funcId) {
#ifdef UMBRA_LLVM_BACKEND
    started_ = true;
    state_ = std::make_shared<SharedState>();
    state_->program = std::move(program);

    // Submit to the persistent thread pool — no pthread_create overhead.
    // The lambda captures shared_ptr<SharedState>, keeping it alive until done.
    CompileThreadPool::instance().submit([state = state_, funcId]() {
        auto start = std::chrono::high_resolution_clock::now();
        try {
            state->compiler = std::make_unique<llvm_backend::LLVMCompiler>();
            state->jitFunc = state->compiler->compile(*state->program, funcId);
            auto end = std::chrono::high_resolution_clock::now();
            state->compileMs = std::chrono::duration<double, std::milli>(end - start).count();
            // Signal ready — the execution thread polls this at morsel boundaries.
            state->ready.store(true, std::memory_order_release);
        } catch (...) {
            state->failed = true;
            // Compilation failed — execution continues with Flying Start.
        }
    });
#else
    (void)program;
    (void)funcId;
    // LLVM not available — AsyncCompiler is a no-op.
    started_ = false;
#endif
}

}  // namespace umbra::execution
