#include "parser/ast_printer.h"

#include <sstream>
#include <string>

namespace umbra::parser {

namespace {

std::string binaryOpName(BinaryOp op) {
    switch (op) {
        case BinaryOp::ADD:
            return "+";
        case BinaryOp::SUB:
            return "-";
        case BinaryOp::MUL:
            return "*";
        case BinaryOp::DIV:
            return "/";
        case BinaryOp::MOD:
            return "%";
        case BinaryOp::AND:
            return "AND";
        case BinaryOp::OR:
            return "OR";
        case BinaryOp::EQ:
            return "=";
        case BinaryOp::NE:
            return "<>";
        case BinaryOp::LT:
            return "<";
        case BinaryOp::LE:
            return "<=";
        case BinaryOp::GT:
            return ">";
        case BinaryOp::GE:
            return ">=";
    }
    return "?";
}

std::string unaryOpName(UnaryOp op) {
    switch (op) {
        case UnaryOp::NEG:
            return "-";
        case UnaryOp::NOT:
            return "NOT";
    }
    return "?";
}

std::string literalTypeName(LiteralType lt) {
    switch (lt) {
        case LiteralType::INTEGER:
            return "INTEGER";
        case LiteralType::STRING:
            return "STRING";
        case LiteralType::DATE:
            return "DATE";
        case LiteralType::INTERVAL:
            return "INTERVAL";
        case LiteralType::DECIMAL:
            return "DECIMAL";
    }
    return "?";
}

void printExprImpl(std::ostringstream& out, const Expr* expr, const std::string& indent,
                   bool isLast) {
    if (expr == nullptr) {
        out << indent << (isLast ? "\\-- " : "|-- ") << "(null)\n";
        return;
    }

    std::string prefix = indent + (isLast ? "\\-- " : "|-- ");
    std::string childIndent = indent + (isLast ? "    " : "|   ");

    switch (expr->kind) {
        case ExprKind::COLUMN_REF: {
            const auto* cr = static_cast<const ColumnRef*>(expr);
            out << prefix << "ColumnRef(";
            if (!cr->tableName.empty()) {
                out << cr->tableName << ".";
            }
            out << cr->columnName << ")\n";
            break;
        }
        case ExprKind::LITERAL: {
            const auto* lit = static_cast<const Literal*>(expr);
            out << prefix << "Literal(" << literalTypeName(lit->literalType);
            if (lit->literalType == LiteralType::INTEGER) {
                out << ", " << lit->intValue;
            } else if (lit->literalType == LiteralType::INTERVAL) {
                out << ", " << lit->intervalValue << " DAY";
            } else if (!lit->strValue.empty()) {
                out << ", \"" << lit->strValue << "\"";
            }
            out << ")\n";
            break;
        }
        case ExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const BinaryExpr*>(expr);
            out << prefix << "BinaryExpr(" << binaryOpName(bin->op) << ")\n";
            printExprImpl(out, bin->left, childIndent, false);
            printExprImpl(out, bin->right, childIndent, true);
            break;
        }
        case ExprKind::UNARY_EXPR: {
            const auto* un = static_cast<const UnaryExpr*>(expr);
            out << prefix << "UnaryExpr(" << unaryOpName(un->op) << ")\n";
            printExprImpl(out, un->operand, childIndent, true);
            break;
        }
        case ExprKind::FUNC_CALL: {
            const auto* fc = static_cast<const FuncCall*>(expr);
            out << prefix << "FuncCall(" << fc->funcName;
            if (fc->isStar) {
                out << ", *";
            }
            out << ")\n";
            for (size_t i = 0; i < fc->args.size(); ++i) {
                printExprImpl(out, fc->args[i], childIndent, i == fc->args.size() - 1);
            }
            break;
        }
        case ExprKind::CAST_EXPR: {
            const auto* ce = static_cast<const CastExpr*>(expr);
            out << prefix << "CastExpr(" << ce->targetTypeName << ")\n";
            printExprImpl(out, ce->expr, childIndent, true);
            break;
        }
        case ExprKind::DATE_ARITH: {
            const auto* da = static_cast<const DateArith*>(expr);
            out << prefix << "DateArith(" << binaryOpName(da->op) << ")\n";
            printExprImpl(out, da->dateExpr, childIndent, false);
            printExprImpl(out, da->interval, childIndent, true);
            break;
        }
        case ExprKind::ALIASED_EXPR: {
            const auto* ae = static_cast<const AliasedExpr*>(expr);
            out << prefix << "AliasedExpr(";
            if (!ae->alias.empty()) {
                out << "alias=\"" << ae->alias << "\"";
            }
            out << ")\n";
            printExprImpl(out, ae->expr, childIndent, true);
            break;
        }
    }
}

}  // namespace

std::string ASTPrinter::print(const SelectStmt& stmt) {
    std::ostringstream out;
    out << "SelectStmt\n";

    // SELECT list
    out << "|-- select_list:\n";
    for (size_t i = 0; i < stmt.selectList.size(); ++i) {
        printExprImpl(out, stmt.selectList[i], "|   ", i == stmt.selectList.size() - 1);
    }

    // FROM
    if (stmt.fromTable != nullptr) {
        out << "|-- from_table: TableRef(\"" << stmt.fromTable->tableName << "\"";
        if (!stmt.fromTable->alias.empty()) {
            out << " AS \"" << stmt.fromTable->alias << "\"";
        }
        out << ")\n";
    }

    // WHERE
    if (stmt.whereClause != nullptr) {
        out << "|-- where_clause:\n";
        printExprImpl(out, stmt.whereClause, "|   ", true);
    }

    // GROUP BY
    if (!stmt.groupBy.empty()) {
        out << "|-- group_by:\n";
        for (size_t i = 0; i < stmt.groupBy.size(); ++i) {
            printExprImpl(out, stmt.groupBy[i], "|   ", i == stmt.groupBy.size() - 1);
        }
    }

    // ORDER BY
    if (!stmt.orderBy.empty()) {
        out << "\\-- order_by:\n";
        for (size_t i = 0; i < stmt.orderBy.size(); ++i) {
            const auto& ob = stmt.orderBy[i];
            std::string prefix = (i == stmt.orderBy.size() - 1) ? "    \\-- " : "    |-- ";
            std::string childIndent = (i == stmt.orderBy.size() - 1) ? "        " : "    |   ";
            out << prefix << "OrderByItem(" << (ob.ascending ? "ASC" : "DESC") << ")\n";
            printExprImpl(out, ob.expr, childIndent, true);
        }
    }

    return out.str();
}

std::string ASTPrinter::printExpr(const Expr* expr) {
    std::ostringstream out;
    printExprImpl(out, expr, "", true);
    return out.str();
}

}  // namespace umbra::parser
