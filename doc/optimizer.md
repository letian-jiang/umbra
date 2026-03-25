# Optimizer Design Document

## 1. Overview

Optimizer 接收 Binder 输出的 Bound Logical Plan，经过规则优化后转换为 Physical Plan，再分解为可执行的 Pipeline。

```
Bound Logical Plan
    │
    ▼
Rule-based Optimization
    ├── Predicate Pushdown
    └── Projection Pushdown
    │
    ▼
Optimized Logical Plan
    │
    ▼
Physical Plan Generation
    │
    ▼
Pipeline Decomposition
    │
    ▼
Pipeline[]  →  交给 Tidy Tuples 逐个编译
```

### 设计原则

- **纯规则，无统计信息**：Q1 是单表查询，不需要 cost model 或 join ordering
- **不修改原树，生成新树**：每个 pass 输入旧树、输出新树，便于调试和 EXPLAIN
- **pass 顺序固定**：predicate pushdown → projection pushdown → physical plan → pipeline decomposition

---

## 2. Predicate Pushdown

### 2.1 目标

将 filter 谓词尽可能下推到靠近数据源的位置，减少上层算子处理的数据量。

### 2.2 规则

```
规则 1: Filter 下推过 Projection
  LogicalFilter(predicate, LogicalProjection(child, exprs))
  → LogicalProjection(LogicalFilter(predicate', child), exprs)
  条件: predicate 只引用 child 的列（不引用 projection 新计算的表达式）
  predicate' = 将 predicate 中的列引用映射回 child 的列

规则 2: Filter 合并到 Scan
  LogicalFilter(predicate, LogicalScan(table))
  → LogicalScan(table, filter=predicate)
  在 physical plan 阶段，filter 集成到 TableScan 算子中

规则 3: Conjunctive 拆分
  LogicalFilter(A AND B, child)
  → LogicalFilter(A, LogicalFilter(B, child))
  拆分后每个子谓词可以独立下推
```

### 2.3 Q1 的效果

初始计划:
```
LogicalSort
  └── LogicalProjection
        └── LogicalAggregate
              └── LogicalFilter(l_shipdate <= ...)
                    └── LogicalScan(lineitem)
```

Filter 已经在 Aggregate 下方，不需要再下推。但后续 physical plan 阶段会将 Filter 合并到 Scan 中（规则 2）。

> **[MVP simplification]** 论文 TPC-H choke points paper §4.2 指出 predicate pushdown 是最重要的优化之一（平均 4x 提升）。但 Q1 只有一个 filter 且已在 scan 之上，pushdown 效果有限。MVP 实现通用规则以备扩展，但 Q1 的受益主要来自 filter 与 scan 的合并。

---

## 3. Projection Pushdown

### 3.1 目标

追踪每个算子实际使用哪些列，将不需要的列从 scan 中裁剪掉。减少内存带宽和 cache 压力。

### 3.2 算法

自顶向下遍历 logical plan，收集每个算子需要的列集合：

```
collectRequiredColumns(node):
    match node:
        LogicalSort(child, orderBy):
            required = columnsReferencedBy(orderBy)
            required ∪= collectRequiredColumns(child)
            return required

        LogicalProjection(child, exprs):
            required = columnsReferencedBy(exprs)
            // projection 截断了上层需求——只需传递 exprs 中引用的列
            child.required = required
            collectRequiredColumns(child)
            return required

        LogicalAggregate(child, groupBy, aggs):
            required = columnsReferencedBy(groupBy)
            required ∪= columnsReferencedBy(aggs)
            child.required = required
            collectRequiredColumns(child)
            return required

        LogicalFilter(child, predicate):
            required = columnsReferencedBy(predicate)
            required ∪= child.required   // 上层需要的列也要传下去
            child.required = required
            collectRequiredColumns(child)
            return required

        LogicalScan(table):
            // 只保留 required 中涉及的列
            node.output_columns = filterColumns(table, node.required)
```

### 3.3 Q1 的效果

lineitem 表有 16 列。Q1 只使用 7 列：

| 用途 | 列 |
|------|-----|
| Filter | l_shipdate (10) |
| Group-by keys | l_returnflag (8), l_linestatus (9) |
| Aggregate inputs | l_quantity (4), l_extendedprice (5), l_discount (6), l_tax (7) |

Projection pushdown 后，scan 只读取这 7 列，跳过其余 9 列。对列存格式，这意味着完全不访问被裁剪列的内存。

---

## 4. Physical Plan Generation

将 logical operator 替换为 physical operator（带有具体执行策略的算子）。

### 4.1 映射规则

| Logical | Physical | 选择理由 |
|---------|----------|---------|
| LogicalScan + LogicalFilter | PhysicalTableScan(filter) | Filter 集成到 scan loop 内，避免物化中间结果 |
| LogicalAggregate | PhysicalHashAggregate | Hash-based，适合无序输入 |
| LogicalProjection | PhysicalProjection | Q1 中最终 projection 计算 AVG = SUM/COUNT |
| LogicalSort | PhysicalSort | Q1 结果集极小（~4 行），任何排序算法都可以 |

### 4.2 Physical Operators

```
PhysicalTableScan:
    table: TableDef*
    columns: vector<ColumnDef*>    // projection pushdown 后的列子集
    filter: BoundExpr*             // 下推的 filter（可选）

PhysicalHashAggregate:
    child: PhysicalOperator*
    group_by_keys: vector<BoundExpr*>
    aggregates: vector<BoundAggExpr*>
    // build side: 从 child 消费 tuple，插入 hash table
    // scan side: 遍历 hash table 输出结果

PhysicalProjection:
    child: PhysicalOperator*
    expressions: vector<BoundExpr*>

PhysicalSort:
    child: PhysicalOperator*
    order_by: vector<BoundOrderByItem>
```

### 4.3 Q1 Physical Plan

```
PhysicalSort
  order_by: [l_returnflag ASC, l_linestatus ASC]
  │
  └── PhysicalProjection
        expressions: [group(0), group(1), agg(0), ..., agg(0)/agg(5), ...]
        │
        └── PhysicalHashAggregate
              group_by: [col(8), col(9)]
              aggregates: [SUM(col(4)), SUM(col(5)), SUM(...), ..., COUNT(*)]
              │
              └── PhysicalTableScan(lineitem)
                    columns: [4, 5, 6, 7, 8, 9, 10]
                    filter: col(10) <= lit(DATE 1998-09-02)
```

---

## 5. Pipeline Decomposition

### 5.1 概念

Pipeline 是一段可以在**不物化中间结果**的情况下连续执行的算子链。Pipeline break 发生在需要物化的算子边界（如 hash table 的 build/scan 之间、sort 的 input/output 之间）。

### 5.2 Pipeline Break 规则

| 算子 | Break 位置 | 原因 |
|------|-----------|------|
| HashAggregate | build 完成后 | 必须先看到所有输入才能扫描结果 |
| Sort | 输入完成后 | 必须先看到所有输入才能排序 |
| HashJoin | build 完成后 | （MVP 不涉及）|

### 5.3 Q1 的 Pipeline 分解

```
Pipeline 1 (build):
  PhysicalTableScan(filter) → PhysicalHashAggregate.buildSide

Pipeline 2 (scan + finalize):
  PhysicalHashAggregate.scanSide → PhysicalProjection → PhysicalSort → Result
```

**Pipeline 1** 是计算密集型：扫描 ~60,000 行，filter 后约 ~58,000 行通过，对每行做 hash + aggregate 更新。

**Pipeline 2** 是轻量型：遍历 hash table 的 ~4 个 entry，计算 AVG，排序，输出。

### 5.4 Pipeline 表示

```
Pipeline:
    source: PhysicalOperator*         // pipeline 的数据来源
    operators: vector<PhysicalOperator*>  // 沿途经过的算子（按数据流顺序）
    sink: PhysicalOperator*           // 终点（hash table build side / result output）
```

每个 Pipeline 被传递给 Tidy Tuples，编译为一个 IR function。

> **[MVP simplification]** 论文中 PhysicalSort 可能在 Pipeline 2 中引入额外的 break（scan → sort → result 变为两个 pipeline）。但 Q1 的排序结果只有 ~4 行，MVP 将 sort 作为 host function 调用而非生成 sort 代码，因此不引入额外 pipeline break。

---

## 6. EXPLAIN Output

Optimizer 在每个阶段保存计划快照，供 EXPLAIN 展示：

| EXPLAIN Level | 内容 |
|--------------|------|
| `LOGICAL` | Binder 输出的 Bound Logical Plan |
| `OPTIMIZED` | 优化后的 Logical Plan（pushdown 后）|
| `PHYSICAL` | Physical Plan（带算子选择）|
| `PIPELINE` | Pipeline 分解结果 |

打印格式（树状缩进）：

```
=== PHYSICAL PLAN ===
PhysicalSort [l_returnflag ASC, l_linestatus ASC]
  PhysicalProjection [10 exprs]
    PhysicalHashAggregate [keys: 2, aggs: 6]
      PhysicalTableScan lineitem [7/16 cols, filter: l_shipdate <= 1998-09-02]

=== PIPELINES ===
Pipeline 0: TableScan → HashAggregate(build)
Pipeline 1: HashAggregate(scan) → Projection → Sort → Result
```

---

## 7. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| Cost-based optimizer | 纯规则，无 cost model | Q1 单表无 join，不需要 |
| Join ordering (DPccp / greedy) | 不实现 | 无 join |
| Subquery flattening | 不实现 | 无 subquery |
| Between composition (rewrite `>=` + `<=` to range) | 不实现 | Q1 只有单侧比较 |
| Semi join reduction | 不实现 | 无 join |
| Dependent group-by elimination | 不实现 | Q1 的 group-by 列独立 |
| Statistics / histograms | 不实现 | |
| Sort 代码生成 | Host function 排序 | 结果集 ~4 行 |
| Multiple physical strategies per logical op | 每种 logical op 固定一种 physical 实现 | Q1 够用 |
