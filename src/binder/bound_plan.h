#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "binder/bound_expr.h"
#include "catalog/catalog.h"
#include "common/types.h"

namespace umbra::binder {

// ── Logical Operator Tags ───────────────────────────────────────────────────

enum class BoundNodeKind {
    SCAN,
    FILTER,
    AGGREGATE,
    SORT,
    PROJECTION,
};

// ── Order By Item ───────────────────────────────────────────────────────────

struct BoundOrderByItem {
    BoundExpr* expr{nullptr};
    bool ascending{true};
};

// ── Logical Operators ───────────────────────────────────────────────────────

/// Base logical operator
struct BoundLogicalOperator {
    BoundNodeKind kind;

    explicit BoundLogicalOperator(BoundNodeKind k) : kind(k) {}
    virtual ~BoundLogicalOperator() = default;

    BoundLogicalOperator(const BoundLogicalOperator&) = default;
    BoundLogicalOperator& operator=(const BoundLogicalOperator&) = default;
    BoundLogicalOperator(BoundLogicalOperator&&) = default;
    BoundLogicalOperator& operator=(BoundLogicalOperator&&) = default;
};

/// Table scan: reads all columns from a table
struct BoundScan : BoundLogicalOperator {
    const TableDef* table;
    std::vector<BoundColumnRef*> outputColumns;

    explicit BoundScan(const TableDef* t) : BoundLogicalOperator(BoundNodeKind::SCAN), table(t) {}
};

/// Filter: applies a boolean predicate
struct BoundFilter : BoundLogicalOperator {
    BoundLogicalOperator* child;
    BoundExpr* predicate;

    BoundFilter(BoundLogicalOperator* ch, BoundExpr* pred)
        : BoundLogicalOperator(BoundNodeKind::FILTER), child(ch), predicate(pred) {}
};

/// Aggregate: group-by keys and aggregate functions
struct BoundAggregate : BoundLogicalOperator {
    BoundLogicalOperator* child;
    std::vector<BoundExpr*> groupBy;
    std::vector<BoundAggCall*> aggregates;

    explicit BoundAggregate(BoundLogicalOperator* ch)
        : BoundLogicalOperator(BoundNodeKind::AGGREGATE), child(ch) {}
};

/// Sort: order-by specification
struct BoundSort : BoundLogicalOperator {
    BoundLogicalOperator* child;
    std::vector<BoundOrderByItem> orderBy;

    explicit BoundSort(BoundLogicalOperator* ch)
        : BoundLogicalOperator(BoundNodeKind::SORT), child(ch) {}
};

/// Projection: final output expressions
struct BoundProjection : BoundLogicalOperator {
    BoundLogicalOperator* child;
    std::vector<BoundExpr*> expressions;
    std::vector<std::string> aliases;

    explicit BoundProjection(BoundLogicalOperator* ch)
        : BoundLogicalOperator(BoundNodeKind::PROJECTION), child(ch) {}
};

}  // namespace umbra::binder
