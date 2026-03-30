#pragma once

#include <cstddef>
#include <cstdint>

namespace umbra::llvm_backend {

// Embedded LLVM bitcode for inlineable host functions.
// Generated at build time by compiling host_inline.cpp → .bc → xxd.
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
static const uint8_t kHostInlineBitcode[] = {
#include "host_inline_bc.inc"
};
// NOLINTEND(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)

static const size_t kHostInlineBitcodeSize = sizeof(kHostInlineBitcode);

}  // namespace umbra::llvm_backend
