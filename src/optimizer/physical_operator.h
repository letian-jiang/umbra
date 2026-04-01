#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "binder/bound_expr.h"
#include "binder/bound_plan.h"
#include "catalog/catalog.h"
#include "common/types.h"

namespace umbra::optimizer {

// ── Physical Operator Tags ──────────────────────────────────────────────────

enum class PhysicalNodeKind {
    TABLE_SCAN,
    HASH_AGGREGATE,
    PROJECTION,
    SORT,
};

// ── Physical Operators ──────────────────────────────────────────────────────

/// Base physical operator
struct PhysicalOperator {
    PhysicalNodeKind kind;

    explicit PhysicalOperator(PhysicalNodeKind k) : kind(k) {}
    virtual ~PhysicalOperator() = default;

    PhysicalOperator(const PhysicalOperator&) = default;
    PhysicalOperator& operator=(const PhysicalOperator&) = default;
    PhysicalOperator(PhysicalOperator&&) = default;
    PhysicalOperator& operator=(PhysicalOperator&&) = default;
};

/// Physical table scan with integrated filter and column pruning.
struct PhysicalTableScan : PhysicalOperator {
    const TableDef* table;
    std::vector<uint32_t> columnIndices;  // pruned column indices
    binder::BoundExpr* filter{nullptr};   // pushed-down filter (optional)

    explicit PhysicalTableScan(const TableDef* t)
        : PhysicalOperator(PhysicalNodeKind::TABLE_SCAN), table(t) {}
};

/// Hash-based aggregation with group-by keys and aggregate functions.
struct PhysicalHashAggregate : PhysicalOperator {
    PhysicalOperator* child;
    std::vector<binder::BoundExpr*> groupByKeys;
    std::vector<binder::BoundAggCall*> aggregates;

    explicit PhysicalHashAggregate(PhysicalOperator* ch)
        : PhysicalOperator(PhysicalNodeKind::HASH_AGGREGATE), child(ch) {}
};

/// Projection: evaluates output expressions.
struct PhysicalProjection : PhysicalOperator {
    PhysicalOperator* child;
    std::vector<binder::BoundExpr*> expressions;
    std::vector<std::string> aliases;
    uint32_t numGroupKeys{0};  // Number of group-by keys from child aggregate (0 if no agg).

    explicit PhysicalProjection(PhysicalOperator* ch)
        : PhysicalOperator(PhysicalNodeKind::PROJECTION), child(ch) {}
};

/// Order by item for physical sort.
struct PhysicalOrderByItem {
    binder::BoundExpr* expr{nullptr};
    bool ascending{true};
};

/// Sort: order-by specification on physical plan.
struct PhysicalSort : PhysicalOperator {
    PhysicalOperator* child;
    std::vector<PhysicalOrderByItem> orderBy;

    explicit PhysicalSort(PhysicalOperator* ch)
        : PhysicalOperator(PhysicalNodeKind::SORT), child(ch) {}
};

// ── Pipeline ────────────────────────────────────────────────────────────────

/// A pipeline is a chain of operators that can execute without materializing
/// intermediate results. Pipeline breaks occur at hash aggregate and sort
/// boundaries.
struct Pipeline {
    PhysicalOperator* source{nullptr};         // data source
    std::vector<PhysicalOperator*> operators;  // operators along the way
    PhysicalOperator* sink{nullptr};           // terminal operator
    std::string description;                   // human-readable label
};

}  // namespace umbra::optimizer
