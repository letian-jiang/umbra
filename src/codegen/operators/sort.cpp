#include "codegen/operators/sort.h"

#include <cassert>

namespace umbra::codegen {

SortTranslator::SortTranslator(const optimizer::PhysicalSort* sort) : sort_(sort) {}

void SortTranslator::produce(CompilationContext& ctx) {
    // For Q1 MVP: Sort is part of Pipeline 1 (scan side).
    // Since the result set is tiny (~4 rows), we treat sort as a passthrough
    // during codegen. The actual sorting is done after pipeline execution
    // by the result collector using a host-side sort.
    // This avoids complex sort code generation for a trivial case.
    assert(child_ != nullptr && "Sort must have a child");
    child_->produce(ctx);
}

void SortTranslator::consume(CompilationContext& ctx, const std::vector<SQLValue>& tuple) {
    // Pass through to parent. Sorting is done post-pipeline on materialized results.
    parent_->consume(ctx, tuple);
}

}  // namespace umbra::codegen
