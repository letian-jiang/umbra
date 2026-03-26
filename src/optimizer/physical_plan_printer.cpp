#include "optimizer/physical_plan_printer.h"

#include <sstream>
#include <string>

#include "binder/bound_plan_printer.h"

namespace umbra::optimizer {

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

std::string aggFuncToString(binder::AggFunc func) {
    switch (func) {
        case binder::AggFunc::SUM:
            return "SUM";
        case binder::AggFunc::COUNT:
            return "COUNT";
    }
    return "?";
}

}  // namespace

std::string PhysicalPlanPrinter::operatorName(const PhysicalOperator* op) {
    if (op == nullptr) {
        return "(null)";
    }
    switch (op->kind) {
        case PhysicalNodeKind::TABLE_SCAN:
            return "TableScan";
        case PhysicalNodeKind::HASH_AGGREGATE:
            return "HashAggregate";
        case PhysicalNodeKind::PROJECTION:
            return "Projection";
        case PhysicalNodeKind::SORT:
            return "Sort";
    }
    return "Unknown";
}

void PhysicalPlanPrinter::printOp(std::string& out, const PhysicalOperator* op, int indent) {
    std::string pad(static_cast<size_t>(indent) * 2, ' ');
    if (op == nullptr) {
        out += pad + "(null)\n";
        return;
    }

    switch (op->kind) {
        case PhysicalNodeKind::TABLE_SCAN: {
            const auto* scan = static_cast<const PhysicalTableScan*>(op);
            out += pad + "PhysicalTableScan " + scan->table->name;
            out += " [" + std::to_string(scan->columnIndices.size()) + "/" +
                   std::to_string(scan->table->columns.size()) + " cols";
            if (scan->filter != nullptr) {
                out += ", filter: " + binder::BoundPlanPrinter::printExpr(scan->filter);
                // Remove trailing newline from expr printer
                if (!out.empty() && out.back() == '\n') {
                    out.pop_back();
                }
            }
            out += "]\n";
            break;
        }
        case PhysicalNodeKind::HASH_AGGREGATE: {
            const auto* agg = static_cast<const PhysicalHashAggregate*>(op);
            out += pad + "PhysicalHashAggregate [keys: " + std::to_string(agg->groupByKeys.size()) +
                   ", aggs: " + std::to_string(agg->aggregates.size()) + "]\n";

            // Print group-by keys summary
            std::string keyPad(static_cast<size_t>(indent + 1) * 2, ' ');
            out += keyPad + "keys: ";
            for (size_t i = 0; i < agg->groupByKeys.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                if (agg->groupByKeys[i]->kind == binder::BoundExprKind::COLUMN_REF) {
                    const auto* col =
                        static_cast<const binder::BoundColumnRef*>(agg->groupByKeys[i]);
                    out += col->columnName;
                } else {
                    out += "expr";
                }
            }
            out += "\n";

            // Print aggregates summary
            out += keyPad + "aggs: ";
            for (size_t i = 0; i < agg->aggregates.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                out += aggFuncToString(agg->aggregates[i]->func);
                out += "(";
                if (agg->aggregates[i]->arg != nullptr) {
                    if (agg->aggregates[i]->arg->kind == binder::BoundExprKind::COLUMN_REF) {
                        const auto* col =
                            static_cast<const binder::BoundColumnRef*>(agg->aggregates[i]->arg);
                        out += col->columnName;
                    } else {
                        out += "expr:" + typeToString(agg->aggregates[i]->arg->resultType);
                    }
                } else {
                    out += "*";
                }
                out += ")";
            }
            out += "\n";

            printOp(out, agg->child, indent + 1);
            break;
        }
        case PhysicalNodeKind::PROJECTION: {
            const auto* proj = static_cast<const PhysicalProjection*>(op);
            out += pad + "PhysicalProjection [" + std::to_string(proj->expressions.size()) +
                   " exprs]\n";
            printOp(out, proj->child, indent + 1);
            break;
        }
        case PhysicalNodeKind::SORT: {
            const auto* sort = static_cast<const PhysicalSort*>(op);
            out += pad + "PhysicalSort [";
            for (size_t i = 0; i < sort->orderBy.size(); ++i) {
                if (i > 0) {
                    out += ", ";
                }
                if (sort->orderBy[i].expr != nullptr &&
                    sort->orderBy[i].expr->kind == binder::BoundExprKind::COLUMN_REF) {
                    const auto* col =
                        static_cast<const binder::BoundColumnRef*>(sort->orderBy[i].expr);
                    out += col->columnName;
                } else {
                    out += "expr";
                }
                out += sort->orderBy[i].ascending ? " ASC" : " DESC";
            }
            out += "]\n";
            printOp(out, sort->child, indent + 1);
            break;
        }
    }
}

std::string PhysicalPlanPrinter::printPlan(const PhysicalOperator* op) {
    std::string result;
    printOp(result, op, 0);
    return result;
}

std::string PhysicalPlanPrinter::printPipelines(const std::vector<Pipeline>& pipelines) {
    std::string result;
    for (size_t i = 0; i < pipelines.size(); ++i) {
        const auto& pipe = pipelines[i];
        result += "Pipeline " + std::to_string(i) + ": ";

        // Source
        result += operatorName(pipe.source);
        if (pipe.source != nullptr && pipe.source->kind == PhysicalNodeKind::TABLE_SCAN) {
            const auto* scan = static_cast<const PhysicalTableScan*>(pipe.source);
            if (scan->filter != nullptr) {
                result += "(filter)";
            }
        }
        if (pipe.source != nullptr && pipe.source->kind == PhysicalNodeKind::HASH_AGGREGATE) {
            result += "(scan)";
        }

        // Intermediate operators
        for (const auto* op : pipe.operators) {
            result += " -> " + operatorName(op);
        }

        // Sink
        if (pipe.sink != pipe.source) {
            result += " -> ";
            if (pipe.sink != nullptr && pipe.sink->kind == PhysicalNodeKind::HASH_AGGREGATE) {
                result += operatorName(pipe.sink) + "(build)";
            } else {
                result += operatorName(pipe.sink);
            }
        }

        result += "\n";
    }
    return result;
}

}  // namespace umbra::optimizer
