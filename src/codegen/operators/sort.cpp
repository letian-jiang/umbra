#include "codegen/operators/sort.h"

#include <cassert>

namespace umbra::codegen {

SortTranslator::SortTranslator(const optimizer::PhysicalSort* sort) : sort_(sort) {}

void SortTranslator::produce(CompilationContext& ctx) {
    // Sort is a passthrough during codegen. The actual sorting is done
    // after pipeline execution by the result collector using a host-side sort.
    assert(child_ != nullptr && "Sort must have a child");
    child_->produce(ctx);
}

void SortTranslator::consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) {
    // Pass through to parent. Sorting is done post-pipeline on materialized results.
    parent_->consume(ctx, tuple);
}

}  // namespace umbra::codegen
