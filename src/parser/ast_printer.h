#pragma once

#include <string>

#include "parser/ast.h"

namespace umbra::parser {

/// AST tree printer for EXPLAIN AST level and debugging.
class ASTPrinter {
   public:
    /// Print the entire SelectStmt as a tree string.
    static std::string print(const SelectStmt& stmt);

    /// Print a single expression.
    static std::string printExpr(const Expr* expr);
};

}  // namespace umbra::parser
