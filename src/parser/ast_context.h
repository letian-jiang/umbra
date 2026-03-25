#pragma once

#include <memory>
#include <vector>

#include "parser/ast.h"

namespace umbra::parser {

/// Arena allocator for AST nodes.
/// All AST nodes are owned by the ASTContext and freed together
/// when the context is destroyed. External code holds raw pointers.
class ASTContext {
   public:
    ASTContext() = default;
    ~ASTContext() = default;

    // Non-copyable, movable
    ASTContext(const ASTContext&) = delete;
    ASTContext& operator=(const ASTContext&) = delete;
    ASTContext(ASTContext&&) = default;
    ASTContext& operator=(ASTContext&&) = default;

    /// Allocate a new AST node of type T and return a raw pointer.
    template <typename T, typename... Args>
    T* alloc(Args&&... args) {
        auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
        auto* raw = ptr.get();
        nodes_.push_back(std::move(ptr));
        return raw;
    }

    /// Allocate a TableRef.
    TableRef* allocTableRef() {
        auto ptr = std::make_unique<TableRef>();
        auto* raw = ptr.get();
        tableRefs_.push_back(std::move(ptr));
        return raw;
    }

    /// Allocate a SelectStmt.
    SelectStmt* allocSelectStmt() {
        auto ptr = std::make_unique<SelectStmt>();
        auto* raw = ptr.get();
        stmts_.push_back(std::move(ptr));
        return raw;
    }

   private:
    std::vector<std::unique_ptr<Expr>> nodes_;
    std::vector<std::unique_ptr<TableRef>> tableRefs_;
    std::vector<std::unique_ptr<SelectStmt>> stmts_;
};

}  // namespace umbra::parser
