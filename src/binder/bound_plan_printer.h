#pragma once

#include <string>

#include "binder/bound_expr.h"
#include "binder/bound_plan.h"

namespace umbra::binder {

/// Prints bound logical plan and expressions as tree strings.
class BoundPlanPrinter {
   public:
    /// Print the entire logical plan tree.
    static std::string print(const BoundLogicalOperator* op);

    /// Print a single bound expression.
    static std::string printExpr(const BoundExpr* expr);
};

}  // namespace umbra::binder
