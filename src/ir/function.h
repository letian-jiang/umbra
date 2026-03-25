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
};

}  // namespace umbra::ir
