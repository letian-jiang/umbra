#pragma once

#include <string>
#include <vector>

#include "optimizer/physical_operator.h"

namespace umbra::optimizer {

/// Prints physical plan and pipeline decomposition as formatted strings.
class PhysicalPlanPrinter {
   public:
    /// Print the physical plan tree.
    static std::string printPlan(const PhysicalOperator* op);

    /// Print the pipeline decomposition.
    static std::string printPipelines(const std::vector<Pipeline>& pipelines);

   private:
    /// Print a single physical operator with indentation.
    static void printOp(std::string& out, const PhysicalOperator* op, int indent);

    /// Get a short name for a physical operator.
    static std::string operatorName(const PhysicalOperator* op);
};

}  // namespace umbra::optimizer
