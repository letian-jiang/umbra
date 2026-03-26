# EXPLAIN Design Document

## 1. Overview

EXPLAIN 展示查询在各个编译阶段的中间表示，用于调试和性能分析。通过在 SQL 前加 `EXPLAIN` 前缀触发，不执行查询。

```sql
EXPLAIN AST      SELECT ...;
EXPLAIN LOGICAL  SELECT ...;
EXPLAIN PHYSICAL SELECT ...;
EXPLAIN PIPELINE SELECT ...;
EXPLAIN IR       SELECT ...;
EXPLAIN ASM      SELECT ...;
EXPLAIN PROFILE  SELECT ...;    -- 这个会执行查询
EXPLAIN ALL      SELECT ...;    -- 打印所有阶段（不含 PROFILE）
```

---

## 2. Stages

### 2.1 AST

Parser 输出的语法树，树形缩进打印。

```
EXPLAIN AST SELECT l_returnflag, SUM(l_quantity) FROM lineitem
            WHERE l_shipdate <= DATE '1998-09-02'
            GROUP BY l_returnflag ORDER BY l_returnflag;

AST:
SelectStmt
├── select_list:
│   ├── ColumnRef(l_returnflag)
│   └── FuncCall(sum, [ColumnRef(l_quantity)]) AS sum_qty
├── from: TableRef(lineitem)
├── where: BinaryExpr(LE, ColumnRef(l_shipdate), Literal(DATE 1998-09-02))
├── group_by: [ColumnRef(l_returnflag)]
└── order_by: [l_returnflag ASC]
```

### 2.2 LOGICAL

Binder 输出的 Bound Logical Plan。每个节点标注输出 schema（列名 + 类型）。

```
EXPLAIN LOGICAL ...

Logical Plan:
LogicalSort [l_returnflag ASC]
  output: [l_returnflag VARCHAR, sum_qty DECIMAL(15,2)]
  └── LogicalProjection
        exprs: [group_ref(0), agg_ref(0)]
        output: [l_returnflag VARCHAR, sum_qty DECIMAL(15,2)]
        └── LogicalAggregate
              group_by: [col(8) l_returnflag]
              aggregates: [SUM(col(4) l_quantity)]
              output: [l_returnflag VARCHAR, sum_qty DECIMAL(15,2)]
              └── LogicalFilter
                    predicate: col(10) <= DATE(1998-09-02)
                    └── LogicalScan(lineitem) [7/16 cols]
```

### 2.3 PHYSICAL

Optimizer 输出的 Physical Plan。标注算子选择和 projection pushdown 结果。

```
EXPLAIN PHYSICAL ...

Physical Plan:
PhysicalSort [l_returnflag ASC]
  └── PhysicalProjection [2 exprs]
        └── PhysicalHashAggregate [keys: 1, aggs: 1]
              └── PhysicalTableScan(lineitem)
                    columns: [4, 8, 10] (3/16)
                    filter: col(10) <= 1998-09-02
```

### 2.4 PIPELINE

Pipeline 分解结果。标注每个 pipeline 的 source/sink 和包含的算子。

```
EXPLAIN PIPELINE ...

Pipelines:
Pipeline 0: [build]
  TableScan(lineitem, filter) → HashAggregate.build

Pipeline 1: [scan]
  HashAggregate.scan → Projection → Sort → Result
```

### 2.5 IR

Umbra IR 的文本表示（见 doc/ir.md §8）。每个 pipeline function 单独打印。

```
EXPLAIN IR ...

=== Pipeline 0 ===
define void @pipeline_0(ptr %0) {
entry:
    %1 = const i64 0
    %2 = const i64 60175
    %3 = const i32 -455
    br loop_header

loop_header:
    %4 = phi2 i64 [entry: %1, loop_continue: %20]
    %5 = cmplt i64 %4, %2
    condbr %5, loop_body, loop_end

loop_body:
    ...

loop_end:
    retvoid
}

=== Pipeline 1 ===
define void @pipeline_1(ptr %0) {
    ...
}
```

### 2.6 ASM

Flying Start 生成的 x86-64 反汇编。使用 asmjit 的内置 formatter 或简单的线性反汇编。

```
EXPLAIN ASM ...

=== Pipeline 0 (198 bytes) ===
0x00:  push   rbp
0x01:  mov    rbp, rsp
0x04:  sub    rsp, 0x40
0x08:  mov    r12, rdi
0x0b:  xor    r9d, r9d
0x0e:  mov    r10, 0xeb0f
       ...
0x42:  cmp    r9, r10
0x45:  jge    0x98
0x47:  mov    eax, [r12+r9*4+0x28]
0x4c:  cmp    eax, -455
0x4f:  jg     0x90
       ...
0x98:  mov    rsp, rbp
0x9b:  pop    rbp
0x9c:  ret
```

### 2.7 PROFILE

执行查询并收集 per-pipeline timing。这是唯一会实际执行查询的 EXPLAIN 模式。

```
EXPLAIN PROFILE ...

Profile (backend: flying-start):
                  plan    codegen  compile  execute  total
Pipeline 0:       -       0.18ms   0.15ms   1.92ms   2.25ms
Pipeline 1:       -       0.04ms   0.02ms   0.01ms   0.07ms
Total:           0.22ms   0.22ms   0.17ms   1.93ms   2.54ms

Result: 4 rows
```

各阶段含义：
- **plan**: parse + bind + optimize（所有 pipeline 共享，只计一次）
- **codegen**: Tidy Tuples 生成 IR
- **compile**: Flying Start IR → x86
- **execute**: pipeline 函数执行时间

### 2.8 ALL

按顺序打印 AST → LOGICAL → PHYSICAL → PIPELINE → IR → ASM，不执行查询。

---

## 3. Implementation

### 3.1 Parsing EXPLAIN

在 Parser 层识别 `EXPLAIN` 前缀，提取 level 关键字，将剩余部分作为 SQL 传递给正常的 parse 流程。

```
parseSQL(input):
    if input starts with "EXPLAIN":
        level = parseExplainLevel()     // AST/LOGICAL/PHYSICAL/.../ALL
        sql = rest of input
        ast = parse(sql)
        return ExplainStmt(level, ast)
    else:
        return parse(input)
```

### 3.2 执行路径

```
executeExplain(level, ast):
    if level >= AST:
        print formatAST(ast)

    bound = bind(ast)
    if level >= LOGICAL:
        print formatLogicalPlan(bound)

    physical = optimize(bound)
    if level >= PHYSICAL:
        print formatPhysicalPlan(physical)

    pipelines = decomposePipelines(physical)
    if level >= PIPELINE:
        print formatPipelines(pipelines)

    if level >= IR:
        for each pipeline:
            irProgram = tidyTuples.compile(pipeline)
            print formatIR(irProgram)

    if level >= ASM:
        for each pipeline:
            flyingStart.compile(irProgram)
            print formatAsm(flyingStart.getCode())

    if level == PROFILE:
        // 完整执行并计时
        executeWithProfiling(pipelines)
```

非 PROFILE 模式下，到达目标 level 后即停止，不继续后续阶段。例如 `EXPLAIN LOGICAL` 只执行 parse + bind，不做 optimize。

ALL 模式执行所有阶段到 ASM，但不执行查询。

### 3.3 Formatter

每个阶段有独立的 formatter，接受对应的数据结构，返回 string：

```
formatAST(ast) → string            // 递归树形打印
formatLogicalPlan(plan) → string   // 递归，每节点标注 output schema
formatPhysicalPlan(plan) → string  // 递归，标注算子类型和参数
formatPipelines(pipelines) → string
formatIR(program) → string         // 遍历 function/block/instruction，按 §2.5 格式输出
formatAsm(codeBuffer) → string     // asmjit Logger 或手动反汇编
```

---

## 4. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| EXPLAIN ANALYZE（PostgreSQL 风格：执行 + per-operator 统计）| PROFILE 只做 per-pipeline 计时 | 无 per-operator 行数/时间 |
| 图形化 plan 可视化 | 纯文本树形缩进 | |
| EXPLAIN 输出到 JSON | 只支持文本 | |
| Cost 信息在 plan 中标注 | 无 cost model，不标注 | |
| per-operator row count | PROFILE 只报告 pipeline 级别 | |
| EXPLAIN IR 打印完整 IR | MVP 只标注 IR 在 pipeline 执行期间生成 | IR 打印需要 codegen 重构暴露中间 IR |
| EXPLAIN ASM 反汇编 | MVP 未实现 ASM 级别 | 需要 asmjit Logger 集成 |
