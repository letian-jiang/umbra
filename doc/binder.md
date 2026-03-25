# Binder Design Document

## 1. Overview

Binder 将 Parser 输出的未解析 AST 转换为 **Bound Logical Plan**——一棵完全类型化、名称已解析、语义已验证的关系代数树。

```
AST (SelectStmt)
    │
    ▼
Binder
    ├── Name Resolution      (列名 → table_idx.column_idx)
    ├── Type Inference        (自底向上推导每个表达式的类型)
    ├── Implicit Cast         (需要时插入 cast 节点)
    ├── Constant Folding      (DATE 算术等编译期可求值的表达式)
    ├── Aggregate Analysis    (分离 group-by keys / aggregate expressions)
    │
    ▼
Bound Logical Plan
    │
    ▼
交给 Optimizer
```

---

## 2. Catalog

Binder 依赖 Catalog 来解析表名和列名。

```
Catalog:
    tables: map<string, TableDef>

TableDef:
    name: string
    columns: vector<ColumnDef>

ColumnDef:
    name: string
    type: SQLType
    column_index: uint32_t       // 在 storage 中的列序号
```

对 Q1，Catalog 中只有一张表 `lineitem`，包含 16 列：

| # | Column Name | SQLType |
|---|------------|---------|
| 0 | l_orderkey | INTEGER |
| 1 | l_partkey | INTEGER |
| 2 | l_suppkey | INTEGER |
| 3 | l_linenumber | INTEGER |
| 4 | l_quantity | DECIMAL(15,2) |
| 5 | l_extendedprice | DECIMAL(15,2) |
| 6 | l_discount | DECIMAL(15,2) |
| 7 | l_tax | DECIMAL(15,2) |
| 8 | l_returnflag | VARCHAR(1) |
| 9 | l_linestatus | VARCHAR(1) |
| 10 | l_shipdate | DATE |
| 11 | l_commitdate | DATE |
| 12 | l_receiptdate | DATE |
| 13 | l_shipinstruct | VARCHAR(25) |
| 14 | l_shipmode | VARCHAR(10) |
| 15 | l_comment | VARCHAR(44) |

---

## 3. Bound Plan Nodes

Binder 的输出不再是 AST，而是关系代数节点树。每个节点都有明确的输出 schema（列名 + 类型列表）。

### 3.1 Logical Operators

```
LogicalScan:
    table: TableDef*
    output_columns: vector<BoundColumnRef>   // 所有列（projection pushdown 后裁剪）

LogicalFilter:
    child: LogicalOperator*
    predicate: BoundExpr*

LogicalProjection:
    child: LogicalOperator*
    expressions: vector<BoundExpr*>          // SELECT list 中的表达式

LogicalAggregate:
    child: LogicalOperator*
    group_by: vector<BoundExpr*>
    aggregates: vector<BoundAggExpr*>

LogicalSort:
    child: LogicalOperator*
    order_by: vector<BoundOrderByItem>
```

### 3.2 Bound Expressions

所有表达式已完成类型推导，携带 `result_type: SQLType`。

```
BoundColumnRef:
    table_index: uint32_t         // Catalog 中的表序号（单表时为 0）
    column_index: uint32_t        // 列序号
    result_type: SQLType

BoundLiteral:
    value: Value                  // union of int64/string/date/...
    result_type: SQLType

BoundBinaryExpr:
    op: BinaryOp
    left: BoundExpr*
    right: BoundExpr*
    result_type: SQLType          // 由类型推导确定

BoundUnaryExpr:
    op: UnaryOp
    operand: BoundExpr*
    result_type: SQLType

BoundCast:
    expr: BoundExpr*
    source_type: SQLType
    target_type: SQLType

BoundAggExpr:
    func: AggFunc                 // SUM / AVG / COUNT
    arg: BoundExpr*               // COUNT(*) 时 arg = nullptr
    result_type: SQLType

BoundOrderByItem:
    expr: BoundExpr*
    ascending: bool
```

---

## 4. Binding Process

### 4.1 Bind FROM

```
bindFrom(ast.from_table):
    TableDef* table = catalog.lookupTable(ast.from_table.table_name)
    if not found: error "table not found"
    // 将 table 注册到当前 scope，使后续列名解析可以找到它
    scope.addTable(table, ast.from_table.alias)
    return LogicalScan(table)
```

### 4.2 Bind WHERE

```
bindWhere(ast.where_clause, scope):
    BoundExpr* predicate = bindExpr(ast.where_clause, scope)
    // 确保结果类型是 BOOLEAN
    if predicate.result_type != BOOLEAN:
        predicate = insertImplicitCast(predicate, BOOLEAN)
    return LogicalFilter(child, predicate)
```

### 4.3 Bind SELECT + GROUP BY

1. 遍历 `group_by` 中的表达式，bind 每个
2. 遍历 `select_list`，bind 每个表达式
3. 对 select_list 中的每个表达式，判断是否包含 aggregate function
4. 验证：非 aggregate 的 select 表达式必须出现在 group_by 中（或是 group_by 表达式的子集）

```
bindSelectAndGroupBy(ast, scope):
    // bind group-by
    for expr in ast.group_by:
        bound = bindExpr(expr, scope)
        groupByExprs.push_back(bound)

    // bind select list, 分离 aggregates
    for aliasedExpr in ast.select_list:
        bound = bindExpr(aliasedExpr.expr, scope)
        if containsAggregate(bound):
            aggExprs.push_back(extractAggregate(bound))
        selectExprs.push_back(bound)

    // 构建 LogicalAggregate
    aggNode = LogicalAggregate(child, groupByExprs, aggExprs)

    // 构建 LogicalProjection (重组 select list 中的 group-by 引用和 aggregate 引用)
    projNode = LogicalProjection(aggNode, selectExprs)
```

### 4.4 Bind ORDER BY

```
bindOrderBy(ast.order_by, scope):
    for item in ast.order_by:
        bound = bindExpr(item.expr, scope)
        orderByItems.push_back(BoundOrderByItem(bound, item.ascending))
    return LogicalSort(child, orderByItems)
```

### 4.5 Bind Expressions (递归)

```
bindExpr(expr, scope):
    match expr:
        ColumnRef(name):
            (table, col) = scope.resolveColumn(name)
            return BoundColumnRef(table.index, col.index, col.type)

        Literal(type, value):
            return BoundLiteral(value, type)

        BinaryExpr(op, left, right):
            l = bindExpr(left, scope)
            r = bindExpr(right, scope)
            // 类型推导 + implicit cast
            (l, r, resultType) = resolveTypes(op, l, r)
            return BoundBinaryExpr(op, l, r, resultType)

        FuncCall("sum", [arg]):
            boundArg = bindExpr(arg, scope)
            resultType = boundArg.result_type   // SUM 保持输入类型
            return BoundAggExpr(SUM, boundArg, resultType)

        FuncCall("avg", [arg]):
            boundArg = bindExpr(arg, scope)
            resultType = DECIMAL(...)           // AVG 结果为 DECIMAL
            return BoundAggExpr(AVG, boundArg, resultType)

        FuncCall("count", []):                  // COUNT(*)
            return BoundAggExpr(COUNT, nullptr, BIGINT)

        DateArith(SUB, dateExpr, intervalExpr):
            // constant fold: 直接计算结果 date
            date = evaluateConstDate(dateExpr)
            interval = evaluateConstInterval(intervalExpr)
            result = date - interval
            return BoundLiteral(result, DATE)
```

---

## 5. Type Inference & Implicit Casts

### 5.1 Type Inference Rules

| Operation | Left Type | Right Type | Result Type | Notes |
|-----------|-----------|-----------|-------------|-------|
| `+` `-` | DECIMAL(p1,s1) | DECIMAL(p2,s2) | DECIMAL(max(p1,p2), max(s1,s2)) | 对齐 scale |
| `*` | DECIMAL(p1,s1) | DECIMAL(p2,s2) | DECIMAL(p1+p2, s1+s2) | scale 相加 |
| `/` | DECIMAL | DECIMAL | DECIMAL | scale 调整 |
| `+` `-` | INTEGER | INTEGER | INTEGER | |
| comparison | T | T | BOOLEAN | 两边类型须一致 |
| comparison | INTEGER | DECIMAL | BOOLEAN | INTEGER 隐式 cast 到 DECIMAL |
| comparison | DATE | DATE | BOOLEAN | |

### 5.2 Implicit Cast Insertion

当二元运算的两边类型不一致时，尝试将一边 cast 到另一边：

```
resolveTypes(op, left, right):
    if left.type == right.type:
        return (left, right, inferResultType(op, left.type))

    // 尝试将较窄类型 cast 到较宽类型
    commonType = findCommonType(left.type, right.type)
    if commonType == null:
        error "incompatible types"

    if left.type != commonType:
        left = BoundCast(left, left.type, commonType)
    if right.type != commonType:
        right = BoundCast(right, right.type, commonType)

    return (left, right, inferResultType(op, commonType))
```

**Cast 优先级**: INTEGER → BIGINT → DECIMAL → (不支持更多)

### 5.3 Q1 中的 Implicit Casts

- `1 - l_discount`：`1` 是 INTEGER literal，`l_discount` 是 DECIMAL(15,2) → 将 `1` cast 为 DECIMAL(15,2)（值变为 100 in scaled representation）
- `1 + l_tax`：同上
- `DATE '1998-12-01' - INTERVAL '90' DAY`：在 binder 阶段 constant fold 为 DATE literal，不需要运行时 cast

---

## 6. Constant Folding

Binder 阶段对**常量表达式**直接求值，减少运行时计算。

### 6.1 折叠范围

| 表达式模式 | 折叠结果 | Q1 示例 |
|-----------|---------|--------|
| DATE literal - INTERVAL | DATE literal | `DATE '1998-12-01' - INTERVAL '90' DAY` → `DATE '1998-09-02'` |
| INTEGER op INTEGER | INTEGER literal | 如果出现 |
| Constant cast | Literal with new type | `1::DECIMAL` → `DECIMAL(100)` |

### 6.2 DATE 计算

```
evaluateConstDate("1998-12-01") → days_since_epoch(1998, 12, 1)
evaluateConstInterval("90", DAY) → 90
result = days_since_epoch(1998, 12, 1) - 90 = days_since_epoch(1998, 9, 2)
```

日期使用距某个 epoch（如 2000-01-01）的天数表示，与 storage 中的 DATE 列格式一致。

---

## 7. Aggregate Analysis

Q1 的 SELECT list 中混合了 group-by 列和 aggregate 表达式，需要分离。

### 7.1 AVG 分解

`AVG(x)` 不是一个原子 aggregate，分解为 `SUM(x)` 和 `COUNT(*)` 两个 aggregate，在最终 projection 中计算 `SUM(x) / COUNT(*)`。

Q1 有三个 AVG：`AVG(l_quantity)`, `AVG(l_extendedprice)`, `AVG(l_discount)`。
其中 `SUM(l_quantity)` 和 `SUM(l_extendedprice)` 已经作为独立的 aggregate 出现在 SELECT list 中——**去重**，不重复计算。

分解后的 aggregate 列表（按 SELECT list 处理顺序生成，去重）：

| # | Aggregate | 来源 |
|---|-----------|------|
| 0 | SUM(l_quantity) | SELECT 直接 + AVG(l_quantity) 共用 |
| 1 | SUM(l_extendedprice) | SELECT 直接 + AVG(l_extendedprice) 共用 |
| 2 | SUM(l_extendedprice * (1 - l_discount)) | SELECT 直接 |
| 3 | SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) | SELECT 直接 |
| 4 | COUNT(*) | AVG(l_quantity) 分解时首次创建，后续 AVG + SELECT 共用 |
| 5 | SUM(l_discount) | AVG(l_discount) 分解 |

注：COUNT(*) 在 AVG(l_quantity) 分解时先于 SUM(l_discount)（AVG(l_discount) 分解时产生）被创建，因此 COUNT(*) 在 index 4，SUM(l_discount) 在 index 5。

Projection 层将 aggregate 结果组合为最终输出：
- `avg_qty` = aggregate[0] / aggregate[4]
- `avg_price` = aggregate[1] / aggregate[4]
- `avg_disc` = aggregate[5] / aggregate[4]

### 7.2 Validation

- 非 aggregate 的 SELECT 表达式必须出现在 GROUP BY 中
- Aggregate 不能嵌套：`SUM(AVG(x))` → error
- GROUP BY 不能包含 aggregate：`GROUP BY SUM(x)` → error

---

## 8. Q1 Bound Logical Plan

```
LogicalSort
  order_by: [BoundColumnRef(0, 8, VARCHAR), ASC],    // l_returnflag
            [BoundColumnRef(0, 9, VARCHAR), ASC]     // l_linestatus
  │
  └── LogicalProjection
        expressions: [
            group_ref(0),                             // l_returnflag
            group_ref(1),                             // l_linestatus
            agg_ref(0),                               // sum_qty
            agg_ref(1),                               // sum_base_price
            agg_ref(2),                               // sum_disc_price
            agg_ref(3),                               // sum_charge
            BinaryExpr(DIV, agg_ref(0), agg_ref(4)),  // avg_qty
            BinaryExpr(DIV, agg_ref(1), agg_ref(4)),  // avg_price
            BinaryExpr(DIV, agg_ref(5), agg_ref(4)),  // avg_disc
            agg_ref(4),                               // count_order
        ]
        │
        └── LogicalAggregate
              group_by: [BoundColumnRef(0, 8), BoundColumnRef(0, 9)]
              aggregates: [
                  SUM(BoundColumnRef(0, 4)),                          // [0] l_quantity
                  SUM(BoundColumnRef(0, 5)),                          // [1] l_extendedprice
                  SUM(MUL(col(5), SUB(lit(1), col(6)))),              // [2] price*(1-disc)
                  SUM(MUL(MUL(col(5), SUB(lit(1), col(6))),
                          ADD(lit(1), col(7)))),                      // [3] ..*(1+tax)
                  COUNT(*)                                            // [4]
                  SUM(BoundColumnRef(0, 6)),                          // [5] l_discount
              ]
              │
              └── LogicalFilter
                    predicate: LE(BoundColumnRef(0, 10, DATE),
                                  BoundLiteral(DATE, 1998-09-02))
                    │
                    └── LogicalScan(lineitem)
```

---

## 9. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| Multi-table scope / join resolution | 单表，scope 只有一张表 | Q1 无 join |
| Subquery binding | 不支持 | Q1 无 subquery |
| Correlated column resolution | 不支持 | |
| HAVING clause | 不支持 | Q1 无 HAVING |
| Window functions | 不支持 | |
| DISTINCT | 不支持 | |
| LIMIT / OFFSET | 不支持 | |
| 完整类型 cast 矩阵 | 只支持 INTEGER→DECIMAL, DATE 计算 | Q1 够用 |
| 表达式 alias 在 ORDER BY 中引用 | 支持（Q1 的 ORDER BY 直接用列名，但结构上需要）| |
