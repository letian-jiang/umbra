#pragma once

#include <stdexcept>
#include <string>

#include "parser/ast.h"
#include "parser/ast_context.h"

namespace umbra::parser {

/// Parse error with source position information.
class ParseError : public std::runtime_error {
   public:
    ParseError(const std::string& message, int cursorPos)
        : std::runtime_error(message), cursorPos_(cursorPos) {}

    [[nodiscard]] int cursorPos() const { return cursorPos_; }

   private:
    int cursorPos_;
};

/// Result of parsing: the AST context owns all nodes, and stmt points
/// to the top-level SelectStmt.
struct ParseResult {
    ASTContext context;
    SelectStmt* stmt{nullptr};
};

/// SQL parser using libpg_query.
/// Parses SQL string into internal AST via PG protobuf conversion.
class Parser {
   public:
    /// Parse a SQL string and return a ParseResult.
    /// Throws ParseError on syntax errors or unsupported SQL features.
    static ParseResult parse(const std::string& sql);
};

}  // namespace umbra::parser
