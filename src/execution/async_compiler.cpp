#include "execution/async_compiler.h"

#include <chrono>
#include <future>

namespace umbra::execution {

AsyncCompiler::~AsyncCompiler() {
    // Wait for background thread to finish if still running.
    // This ensures the LLVMCompiler instance isn't destroyed while in use.
    if (future_.valid()) {
        future_.wait();
    }
}

void AsyncCompiler::startAsync(std::shared_ptr<ir::Program> program, ir::FunctionId funcId) {
#ifdef UMBRA_LLVM_BACKEND
    started_ = true;
    program_ = std::move(program);

    // Launch LLVM compilation on a background thread.
    future_ = std::async(std::launch::async, [this, funcId]() {
        auto start = std::chrono::high_resolution_clock::now();
        try {
            compiler_ = std::make_unique<llvm_backend::LLVMCompiler>();
            jitFunc_ = compiler_->compile(*program_, funcId);
            auto end = std::chrono::high_resolution_clock::now();
            compileMs_ = std::chrono::duration<double, std::milli>(end - start).count();
            // Signal ready — the execution thread polls this at morsel boundaries.
            ready_.store(true, std::memory_order_release);
        } catch (...) {
            failed_ = true;
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
