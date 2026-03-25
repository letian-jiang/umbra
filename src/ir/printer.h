#pragma once

#include <string>

#include "ir/program.h"

namespace umbra::ir {

/// IRPrinter — prints IR programs in human-readable text format.
/// Used for EXPLAIN IR level and debugging.
class IRPrinter {
   public:
    /// Print the entire program.
    static std::string print(const Program& program);

    /// Print a single function.
    static std::string printFunction(const Program& program, FunctionId funcId);

    /// Print a single instruction.
    static std::string printInstruction(const Program& program, IROp offset);
};

}  // namespace umbra::ir
