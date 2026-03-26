#include "binder/bound_plan_printer.h"

#include <sstream>
#include <string>

namespace umbra::binder {

namespace {

std::string typeToString(SQLType type) {
    switch (type.id) {
        case SQLTypeId::INTEGER:
            return "INTEGER";
        case SQLTypeId::BIGINT:
            return "BIGINT";
        case SQLTypeId::DATE:
            return "DATE";
        case SQLTypeId::DECIMAL:
            return "DECIMAL(" + std::to_string(type.precision) + "," + std::to_string(type.scale) +
                   ")";
        case SQLTypeId::CHAR:
            return "CHAR(" + std::to_string(type.maxLen) + ")";
        case SQLTypeId::VARCHAR:
            return "VARCHAR(" + std::to_string(type.maxLen) + ")";
        case SQLTypeId::BOOLEAN:
            return "BOOLEAN";
    }
    return "UNKNOWN";
}

std::string binaryOpToString(parser::BinaryOp op) {
    switch (op) {
        case parser::BinaryOp::ADD:
            return "+";
        case parser::BinaryOp::SUB:
            return "-";
        case parser::BinaryOp::MUL:
            return "*";
        case parser::BinaryOp::DIV:
            return "/";
        case parser::BinaryOp::MOD:
            return "%";
        case parser::BinaryOp::AND:
            return "AND";
        case parser::BinaryOp::OR:
            return "OR";
        case parser::BinaryOp::EQ:
            return "=";
        case parser::BinaryOp::NE:
            return "<>";
        case parser::BinaryOp::LT:
            return "<";
        case parser::BinaryOp::LE:
            return "<=";
        case parser::BinaryOp::GT:
            return ">";
        case parser::BinaryOp::GE:
            return ">=";
    }
    return "?";
}

std::string unaryOpToString(parser::UnaryOp op) {
    switch (op) {
        case parser::UnaryOp::NEG:
            return "-";
        case parser::UnaryOp::NOT:
            return "NOT";
    }
    return "?";
}

std::string aggFuncToString(AggFunc func) {
    switch (func) {
        case AggFunc::SUM:
            return "SUM";
        case AggFunc::COUNT:
            return "COUNT";
    }
    return "?";
}

void printExprImpl(std::ostringstream& out, const BoundExpr* expr, int indent) {
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    if (expr == nullptr) {
        out << pad << "(null)\n";
        return;
    }

    switch (expr->kind) {
        case BoundExprKind::COLUMN_REF: {
            const auto* col = static_cast<const BoundColumnRef*>(expr);
            out << pad << "ColumnRef(" << col->columnName << " [" << col->tableIndex << "."
                << col->columnIndex << "] : " << typeToString(col->resultType) << ")\n";
            break;
        }
        case BoundExprKind::LITERAL: {
            const auto* lit = static_cast<const BoundLiteral*>(expr);
            out << pad << "Literal(";
            if (lit->resultType.id == SQLTypeId::VARCHAR) {
                out << "'" << lit->strValue << "'";
            } else if (lit->resultType.id == SQLTypeId::CHAR && lit->resultType.maxLen == 1) {
                out << "'" << static_cast<char>(lit->intValue) << "'";
            } else {
                out << lit->intValue;
            }
            out << " : " << typeToString(lit->resultType) << ")\n";
            break;
        }
        case BoundExprKind::BINARY_EXPR: {
            const auto* bin = static_cast<const BoundBinaryExpr*>(expr);
            out << pad << "BinaryExpr(" << binaryOpToString(bin->op) << " : "
                << typeToString(bin->resultType) << ")\n";
            printExprImpl(out, bin->left, indent + 1);
            printExprImpl(out, bin->right, indent + 1);
            break;
        }
        case BoundExprKind::UNARY_EXPR: {
            const auto* un = static_cast<const BoundUnaryExpr*>(expr);
            out << pad << "UnaryExpr(" << unaryOpToString(un->op) << " : "
                << typeToString(un->resultType) << ")\n";
            printExprImpl(out, un->operand, indent + 1);
            break;
        }
        case BoundExprKind::AGG_CALL: {
            const auto* agg = static_cast<const BoundAggCall*>(expr);
            out << pad << "AggCall(" << aggFuncToString(agg->func) << " : "
                << typeToString(agg->resultType) << ")\n";
            if (agg->arg != nullptr) {
                printExprImpl(out, agg->arg, indent + 1);
            } else {
                out << pad << "  (*)\n";
            }
            break;
        }
        case BoundExprKind::CAST: {
            const auto* cast = static_cast<const BoundCast*>(expr);
            out << pad << "Cast(" << typeToString(cast->sourceType) << " -> "
                << typeToString(cast->resultType) << ")\n";
            printExprImpl(out, cast->expr, indent + 1);
            break;
        }
        case BoundExprKind::AGG_REF: {
            const auto* ref = static_cast<const BoundAggRef*>(expr);
            out << pad << "AggRef(" << ref->aggIndex << " : " << typeToString(ref->resultType)
                << ")\n";
            break;
        }
        case BoundExprKind::GROUP_REF: {
            const auto* ref = static_cast<const BoundGroupRef*>(expr);
            out << pad << "GroupRef(" << ref->groupIndex << " : " << typeToString(ref->resultType)
                << ")\n";
            break;
        }
    }
}

void printOpImpl(std::ostringstream& out, const BoundLogicalOperator* op, int indent) {
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    if (op == nullptr) {
        out << pad << "(null)\n";
        return;
    }

    switch (op->kind) {
        case BoundNodeKind::SCAN: {
            const auto* scan = static_cast<const BoundScan*>(op);
            out << pad << "Scan(" << scan->table->name << ")\n";
            break;
        }
        case BoundNodeKind::FILTER: {
            const auto* filter = static_cast<const BoundFilter*>(op);
            out << pad << "Filter\n";
            out << pad << "  predicate:\n";
            printExprImpl(out, filter->predicate, indent + 2);
            printOpImpl(out, filter->child, indent + 1);
            break;
        }
        case BoundNodeKind::AGGREGATE: {
            const auto* agg = static_cast<const BoundAggregate*>(op);
            out << pad << "Aggregate\n";
            out << pad << "  group_by:\n";
            for (const auto* gbExpr : agg->groupBy) {
                printExprImpl(out, gbExpr, indent + 2);
            }
            out << pad << "  aggregates:\n";
            for (const auto* aggExpr : agg->aggregates) {
                printExprImpl(out, aggExpr, indent + 2);
            }
            printOpImpl(out, agg->child, indent + 1);
            break;
        }
        case BoundNodeKind::SORT: {
            const auto* sort = static_cast<const BoundSort*>(op);
            out << pad << "Sort\n";
            out << pad << "  order_by:\n";
            for (const auto& item : sort->orderBy) {
                printExprImpl(out, item.expr, indent + 2);
                out << pad << "    " << (item.ascending ? "ASC" : "DESC") << "\n";
            }
            printOpImpl(out, sort->child, indent + 1);
            break;
        }
        case BoundNodeKind::PROJECTION: {
            const auto* proj = static_cast<const BoundProjection*>(op);
            out << pad << "Projection\n";
            out << pad << "  expressions:\n";
            for (size_t i = 0; i < proj->expressions.size(); ++i) {
                if (i < proj->aliases.size() && !proj->aliases[i].empty()) {
                    out << pad << "    [" << proj->aliases[i] << "]:\n";
                }
                printExprImpl(out, proj->expressions[i], indent + 2);
            }
            printOpImpl(out, proj->child, indent + 1);
            break;
        }
    }
}

}  // namespace

std::string BoundPlanPrinter::print(const BoundLogicalOperator* op) {
    std::ostringstream out;
    printOpImpl(out, op, 0);
    return out.str();
}

std::string BoundPlanPrinter::printExpr(const BoundExpr* expr) {
    std::ostringstream out;
    printExprImpl(out, expr, 0);
    return out.str();
}

}  // namespace umbra::binder
