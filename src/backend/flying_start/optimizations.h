#pragma once

#include <cstdint>

#include "backend/flying_start/live_span.h"
#include "backend/flying_start/register_alloc.h"
#include "ir/instruction.h"
#include "ir/opcode.h"
#include "ir/program.h"

namespace umbra::flying_start {

/// Optimization decisions for the Flying Start JIT.
/// All four on-the-fly optimizations are computed here and consumed by
/// the translator during code emission.

/// GEP address info for lazy address calculation (Optimization 3).
struct GepAddress {
    ir::IROp baseOp;      ///< Base pointer operand.
    ir::IROp indexOp;     ///< Index operand.
    ir::TypeId elemType;  ///< Element type (determines scale factor).
};

/// Check if a value should use a register (vs stack).
/// Implements the resultReg heuristic from the design doc (Figure 10).
/// Returns true if the value should be placed in a register.
bool shouldAllocateRegister(const LiveSpanAnalysis& lsa, ir::IROp op, uint32_t currentBlockOff,
                            const RegisterAllocator& regAlloc);

/// Check if a comparison can be fused with a subsequent condbr.
/// Returns true if the comparison result is only used by a single condbr.
bool canFuseComparisonBranch(const ir::Program& program, const LiveSpanAnalysis& lsa,
                             ir::IROp cmpOp);

/// Check if a GEP can be deferred (lazy address calculation).
/// Returns true if the GEP result is only used by load/store instructions.
bool canDeferGep(const ir::Program& program, const LiveSpanAnalysis& lsa, ir::IROp gepOp);

/// Get the x86 condition code for a comparison opcode.
/// Returns the asmjit condition code (e.g., cc::kE for CMP_EQ).
uint8_t comparisonConditionCode(ir::Opcode opcode);

/// Get the inverse x86 condition code.
uint8_t inverseConditionCode(uint8_t cc);

}  // namespace umbra::flying_start
