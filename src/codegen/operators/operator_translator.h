#pragma once

#include <vector>

#include "codegen/api/codegen_context.h"
#include "codegen/sql_values/sql_value.h"
#include "optimizer/physical_operator.h"

namespace umbra::codegen {

/// Forward declare
class OperatorTranslator;

/// CompilationContext — shared context for operator translators during codegen.
/// Contains references to the IR builder, catalog, and runtime pointers.
struct CompilationContext {
    CodegenContext& codegen;
    Ptr<Int8> runtimeCtx;  // Pointer to RuntimeContext passed as function arg
    Int64 startRow;        // Morsel start index (function arg 1)
    Int64 endRow;          // Morsel end index (function arg 2)

    explicit CompilationContext(CodegenContext& cg) : codegen(cg) {}
};

/// OperatorTranslator — base class for push-based code generation.
///
/// Implements the produce/consume protocol from Neumann 2011:
/// - produce(ctx): emit code to generate tuples (called top-down)
/// - consume(ctx, tuple): process a tuple from a child (called bottom-up)
///
/// Each physical operator subclasses this and implements the protocol.
class OperatorTranslator {
   public:
    virtual ~OperatorTranslator() = default;

    OperatorTranslator(const OperatorTranslator&) = delete;
    OperatorTranslator& operator=(const OperatorTranslator&) = delete;
    OperatorTranslator(OperatorTranslator&&) = delete;
    OperatorTranslator& operator=(OperatorTranslator&&) = delete;

    /// Emit code to produce tuples. Drives the child's produce and
    /// eventually calls parent->consume for each output tuple.
    virtual void produce(CompilationContext& ctx) = 0;

    /// Process a tuple from a child operator.
    /// @param ctx The compilation context.
    /// @param tuple The tuple (vector of SQLValues) produced by the child.
    virtual void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) = 0;

    /// Set the parent operator (the one that consumes our output).
    void setParent(OperatorTranslator* parent) { parent_ = parent; }

    /// Get the parent operator.
    [[nodiscard]] OperatorTranslator* parent() const { return parent_; }

   protected:
    OperatorTranslator() = default;

    OperatorTranslator* parent_{nullptr};
};

}  // namespace umbra::codegen
