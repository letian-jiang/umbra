# Parser Design Document

## 1. Overview

Parser 将 SQL 字符串转换为内部 AST。分两步：先用 libpg_query 得到 PostgreSQL 的 parse tree，再转换为我们自己的 AST 节点。

```
SQL string
    │
    ▼
libpg_query  →  PG parse tree (protobuf)
    │
    ▼
PG-to-AST converter  →  internal AST
    │
    ▼
交给 Binder
```

---

## 2. libpg_query

### 2.1 选型理由

- 包装了 PostgreSQL 的真实 parser，SQL 兼容性高
- 纯 C 库，无外部依赖，编译快
- 输出为 protobuf 结构，解析方便
- 不需要自己写 lexer/parser，减少 MVP 工作量

### 2.2 调用接口

```cpp
#include <libpg_query/pg_query.h>

PgQueryProtobufParseResult result = pg_query_parse_protobuf(sql_string);
// result.parse_tree  → protobuf bytes
// result.error       → 如果有语法错误
pg_query_free_protobuf_parse_result(result);
```

parse 结果是 `pg_query.proto` 定义的 protobuf message，根节点是 `ParseResult`，包含一个 `stmts` 列表。

---

## 3. Internal AST

我们定义一套轻量 AST 节点，只覆盖 Q1 用到的 SQL 构造。PG parse tree 包含大量我们不需要的节点类型，转换层负责过滤。

### 3.1 Node Types

**Statement:**

```
SelectStmt:
    select_list: vector<Expr*>       // SELECT 子句中的表达式
    from_table: TableRef*            // FROM 子句
    where_clause: Expr*              // WHERE 子句（可选）
    group_by: vector<Expr*>          // GROUP BY 子句
    order_by: vector<OrderByItem>    // ORDER BY 子句
```

**Table Reference:**

```
TableRef:
    table_name: string               // "lineitem"
    alias: string                    // 可选别名
```

**Expressions:**

```
ColumnRef:
    table_name: string               // 可选，如 "l" in "l.l_shipdate"
    column_name: string              // "l_shipdate"

Literal:
    type: LiteralType                // INTEGER / STRING / DATE / INTERVAL / DECIMAL
    int_value: int64_t
    str_value: string

BinaryExpr:
    op: BinaryOp                     // ADD / SUB / MUL / DIV / AND / OR /
                                     // EQ / NE / LT / LE / GT / GE
    left: Expr*
    right: Expr*

UnaryExpr:
    op: UnaryOp                      // NEG / NOT
    operand: Expr*

FuncCall:
    func_name: string                // "sum" / "avg" / "count"
    args: vector<Expr*>              // 参数列表（COUNT(*) 时 args 为空）
    is_star: bool                    // COUNT(*) 标记

CastExpr:
    expr: Expr*
    target_type: SQLType

DateArith:
    date_expr: Expr*                 // DATE '1998-12-01'
    interval: Expr*                  // INTERVAL '90' DAY
    op: ADD / SUB
```

**Order By:**

```
OrderByItem:
    expr: Expr*
    ascending: bool                  // 默认 true
```

**Alias:**

```
AliasedExpr:
    expr: Expr*
    alias: string                    // "sum_qty", "avg_price" 等
```

### 3.2 Node 所有权

所有 AST 节点由 `ASTContext` 拥有（arena allocator），外部持有裸指针。AST 的生命周期 = ASTContext 的生命周期。查询结束后统一释放，无逐节点 delete。

---

## 4. PG-to-AST Conversion

递归遍历 PG protobuf 节点，转换为 internal AST。

### 4.1 Q1 涉及的 PG 节点类型

| PG Node | 转换目标 | 说明 |
|---------|---------|------|
| `SelectStmt` | `SelectStmt` | 顶层 SELECT |
| `ResTarget` | `AliasedExpr` | SELECT list 中的每一项 |
| `ColumnRef` | `ColumnRef` | 列引用 |
| `A_Const` (integer) | `Literal` | 整数常量，如 `1`, `90` |
| `A_Const` (string) | `Literal` | 字符串常量 |
| `TypeCast` | 识别 `DATE '...'` / `INTERVAL '...'` 模式 | |
| `A_Expr` (op) | `BinaryExpr` / `UnaryExpr` | 算术和比较 |
| `FuncCall` | `FuncCall` | SUM, AVG, COUNT |
| `SortBy` | `OrderByItem` | ORDER BY |
| `RangeVar` | `TableRef` | FROM 表引用 |

### 4.2 DATE / INTERVAL 处理

Q1 中的 `DATE '1998-12-01' - INTERVAL '90' DAY` 在 PG parse tree 中表现为：

```
A_Expr (op: "-")
  ├── TypeCast(arg: A_Const("1998-12-01"), typeName: "date")
  └── TypeCast(arg: A_Const("90"), typeName: "interval", typmods: [DAY])
```

转换时识别这个模式，生成 `DateArith` 节点。这个表达式在 binder 阶段会被 constant fold 为一个 `Literal{type: DATE, value: days_since_epoch("1998-09-02")}`。

### 4.3 Error Handling

- libpg_query 返回语法错误 → 直接报错，包含行号和位置
- PG 节点类型不在我们支持的范围内 → 报 "unsupported SQL feature"
- 不做语义检查（那是 Binder 的职责）

---

## 5. Q1 AST Example

```sql
SELECT l_returnflag, l_linestatus,
       SUM(l_quantity) AS sum_qty, ...
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

转换后的 AST：

```
SelectStmt
├── select_list:
│   ├── AliasedExpr(ColumnRef("l_returnflag"), null)
│   ├── AliasedExpr(ColumnRef("l_linestatus"), null)
│   ├── AliasedExpr(FuncCall("sum", [ColumnRef("l_quantity")]), "sum_qty")
│   ├── AliasedExpr(FuncCall("sum", [ColumnRef("l_extendedprice")]), "sum_base_price")
│   ├── AliasedExpr(FuncCall("sum", [BinaryExpr(MUL,
│   │       ColumnRef("l_extendedprice"),
│   │       BinaryExpr(SUB, Literal(1), ColumnRef("l_discount")))
│   │   ]), "sum_disc_price")
│   ├── ... (sum_charge, avg_qty, avg_price, avg_disc, count_order)
├── from_table: TableRef("lineitem")
├── where_clause:
│   └── BinaryExpr(LE,
│         ColumnRef("l_shipdate"),
│         DateArith(SUB, Literal(DATE, "1998-12-01"), Literal(INTERVAL, 90, DAY)))
├── group_by:
│   ├── ColumnRef("l_returnflag")
│   └── ColumnRef("l_linestatus")
└── order_by:
    ├── OrderByItem(ColumnRef("l_returnflag"), ASC)
    └── OrderByItem(ColumnRef("l_linestatus"), ASC)
```

---

## 6. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| 完整 SQL 语法（JOIN, subquery, CTE, UNION, ...） | 只支持 single-table SELECT + WHERE + GROUP BY + ORDER BY | Q1 够用 |
| Expression alias 解析（ORDER BY 引用 SELECT alias）| 支持（Q1 需要）| |
| Multiple statements | 只处理第一条 | |
| Schema-qualified names (`schema.table`) | 不支持，只用 table name | |
| String escape / Unicode | 最基本的处理 | Q1 无复杂字符串 |
