#pragma once

#include <cstdint>
#include <vector>

#include "codegen/operators/operator_translator.h"
#include "codegen/sql_values/sql_value.h"

namespace umbra::codegen {

/// ResultWriterTranslator — pipeline sink that writes output tuples to a buffer.
///
/// Emits IR code to write each tuple value to a result buffer.
/// The buffer is laid out as a flat array: buffer[row * numCols + col].
/// A row counter is maintained in the RuntimeContext to track the current row.
class ResultWriterTranslator : public OperatorTranslator {
   public:
    /// @param numOutputCols Number of output columns expected.
    explicit ResultWriterTranslator(uint32_t numOutputCols);

    void produce(CompilationContext& ctx) override;
    void consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) override;

    /// Set the child translator.
    void setChild(OperatorTranslator* child) { child_ = child; }

   private:
    uint32_t numOutputCols_;
    OperatorTranslator* child_{nullptr};
};

}  // namespace umbra::codegen
