#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ir/opcode.h"

namespace umbra::ir {

/// FunctionId — index into the program's function table.
using FunctionId = uint32_t;

/// FunctionEntry — hot-path structure: only the entry block offset.
/// Paper-faithful: "Each function only contains the offset of the first basic block."
struct FunctionEntry {
    uint32_t entryBlockOffset;  // byte offset into block buffer
};

/// FunctionMeta — side table for debug/backend info.
struct FunctionMeta {
    uint32_t numParams;
    TypeId returnType;
    std::string name;
    std::vector<TypeId> paramTypes;

    /// C ABI function pointer for direct calls (LLVM backend only).
    /// When non-null, the LLVM backend calls this directly instead of going
    /// through the umbraCallHost trampoline.
    void* directPtr = nullptr;

    /// If true, the LLVM backend will inline this function's body (from
    /// embedded bitcode) at all call sites.  Interpreter and Flying Start
    /// ignore this flag.
    bool inlineable = false;
};

}  // namespace umbra::ir
