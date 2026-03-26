# EXPLAIN Design Document

## 1. Overview

EXPLAIN 展示查询在各个编译阶段的中间表示，用于调试和性能分析。通过在 SQL 前加 `EXPLAIN` 前缀触发。除 PROFILE 外均不执行查询。

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

Umbra IR 的文本表示（见 doc/ir.md §8）。通过 `PipelineExecutor::generateIR()` 对每个 pipeline 独立执行 codegen（Tidy Tuples produce/consume），生成 `ir::Program`，再用 `IRPrinter::print()` 格式化输出。不执行查询。

```
EXPLAIN IR ...

IR:
=== Pipeline 0: build ===
define void @pipeline_build(ptr %0) {
entry:
    %0 = funarg ptr %0
    %6 = bitcast ptr %0
    %12 = load ptr %6
    ...
    br void bb1

bb1:
    %238 = phi2 i64 [entry: %222], [bb7: %2080]
    ...
    condbr void %393, bb3, bb4
    ...
}

; host function @ht_lookup
; host function @ht_insert

=== Pipeline 1: scan ===
define void @pipeline_scan(ptr %0) {
    ...
}
```

### 2.6 ASM

Flying Start 生成的 x86-64 汇编。通过 `PipelineExecutor::generateASM()` 对每个 pipeline 执行 codegen + JIT 编译，使用 asmjit 的 `StringLogger` 在编译过程中捕获汇编文本。始终使用 Flying Start 后端（与当前 `.backend` 设置无关）。不执行查询。

```
EXPLAIN ASM ...

ASM (x86-64, Flying Start):
=== Pipeline 0: build ===
push rbp
mov rbp, rsp
sub rsp, 4096
push rbx
...
L0:
mov qword ptr [rbp-8], rdi
...
crc32 rax, rcx
...
call rax
...

=== Pipeline 1: scan ===
push rbp
mov rbp, rsp
...
idiv rcx
...
ret
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

按顺序打印 AST → LOGICAL → PHYSICAL → PIPELINE，不执行查询。IR 和 ASM 需要单独通过 `EXPLAIN IR` / `EXPLAIN ASM` 查看（因为需要 codegen/compile）。ALL 模式下 IR 位置显示提示信息 `(use EXPLAIN IR to see generated IR)`。

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
runExplain(level, sql, catalog, table, backend):
    // Parse
    ast = parse(sql)
    if level == AST: return formatAST(ast)

    // Bind
    bound = bind(ast)
    if level == LOGICAL: return formatLogicalPlan(bound)

    // Optimize
    physical = optimize(bound)
    if level == PHYSICAL: return formatPhysicalPlan(physical)
    if level == PIPELINE: return formatPipelines(physical.pipelines)

    // IR: codegen only (no execution)
    if level == IR:
        executor = PipelineExecutor(backend)
        return executor.generateIR(physical)

    // ASM: codegen + compile (always Flying Start, no execution)
    if level == ASM:
        executor = PipelineExecutor(FLYING_START)
        return executor.generateASM(physical)

    // PROFILE: full execution with per-pipeline timing
    if level == PROFILE:
        executor = PipelineExecutor(backend)
        executor.enableProfiling()
        executor.execute(physical, table, collector)
        return formatProfile(executor.profiles())

    // ALL: print AST → LOGICAL → PHYSICAL → PIPELINE → IR hint
    // IR/ASM not included (require separate codegen)
```

非 PROFILE 模式下，到达目标 level 后即停止，不继续后续阶段。例如 `EXPLAIN LOGICAL` 只执行 parse + bind，不做 optimize。

`EXPLAIN IR` 和 `EXPLAIN ASM` 需要执行 codegen 阶段（Tidy Tuples produce/consume），但不执行生成的代码。`EXPLAIN ASM` 始终使用 Flying Start 后端，因为 Interpreter 不生成机器码。

### 3.3 Formatter / IR & ASM Generation

每个阶段有独立的 formatter 或生成方法：

```
formatAST(ast) → string                    // parser::ASTPrinter::print()
formatLogicalPlan(plan) → string           // binder::BoundPlanPrinter::print()
formatPhysicalPlan(plan) → string          // optimizer::PhysicalPlanPrinter::printPlan()
formatPipelines(pipelines) → string        // optimizer::PhysicalPlanPrinter::printPipelines()
PipelineExecutor::generateIR(result)       // codegen → ir::IRPrinter::print(program)
PipelineExecutor::generateASM(result)      // codegen → FlyingStartCompiler::compileWithLog()
```

**IR 生成流程** (`generateIR`):
1. 为每个 pipeline 创建 operator translator 链
2. 创建 `CodegenContext`，调用 `produce()` 生成 IR
3. `finalize()` 得到 `ir::Program`
4. 用 `IRPrinter::print()` 格式化为文本

**ASM 生成流程** (`generateASM`):
1. 同 IR 流程的 1-3 步
2. 创建 `FlyingStartCompiler`，调用 `compileWithLog(program, funcId, asmOutput)`
3. `compileWithLog` 在 asmjit `CodeHolder` 上挂载 `StringLogger`
4. 编译过程中 asmjit 自动将每条 x86-64 指令写入 logger
5. 编译完成后从 `StringLogger::data()` 提取汇编文本

---

## 4. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| EXPLAIN ANALYZE（PostgreSQL 风格：执行 + per-operator 统计）| PROFILE 只做 per-pipeline 计时 | 无 per-operator 行数/时间 |
| 图形化 plan 可视化 | 纯文本树形缩进 | |
| EXPLAIN 输出到 JSON | 只支持文本 | |
| Cost 信息在 plan 中标注 | 无 cost model，不标注 | |
| per-operator row count | PROFILE 只报告 pipeline 级别 | |
| ~~EXPLAIN IR 打印完整 IR~~ | ~~MVP 只标注 IR 在 pipeline 执行期间生成~~ | **已实现** — `PipelineExecutor::generateIR()` |
| ~~EXPLAIN ASM 反汇编~~ | ~~MVP 未实现 ASM 级别~~ | **已实现** — `FlyingStartCompiler::compileWithLog()` + asmjit `StringLogger` |
