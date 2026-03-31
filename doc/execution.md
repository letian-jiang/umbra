# Execution Design Document

## 1. Overview

Execution 层负责将 optimizer 输出的 physical plan 分解为 pipeline，对每个 pipeline 执行 codegen → compile → execute 三阶段流程，并收集结果。

```
OptimizeResult (physical plan + pipeline decomposition)
     │
     ▼
PipelineExecutor
     │
     ├── Pipeline 0 (build): TableScan → HashAggregate
     │    ├── Codegen: Tidy Tuples → IR Program
     │    ├── Compile: IR → native code (via selected backend)
     │    └── Execute: call pipeline function with RuntimeContext
     │
     ├── Pipeline 1 (scan): HashAggregate(scan) → Proj → Sort → ResultWriter
     │    ├── Codegen → Compile → Execute (same flow)
     │    └── Host-side sort on materialized results
     │
     └── ResultCollector: buffer → materialize → format
```

### 设计原则

1. **Pipeline 串行执行**：Pipeline 0 完成后才开始 Pipeline 1（build→scan 依赖）
2. **Codegen 与执行分离**：`codegenAllPipelines()` 可独立调用（用于 EXPLAIN IR/ASM）
3. **Backend 透明**：execution 层不关心 backend 实现细节，只调用 `compile()` + `execute()`
4. **RuntimeContext 作为唯一通信通道**：pipeline function 与 host 之间通过 RuntimeContext 传递所有状态

---

## 2. Pipeline 分解

### 2.1 Pipeline Breaker

Hash aggregation 是 pipeline breaker：build side 和 scan side 必须分成两个 pipeline。Build pipeline 将所有行插入 hash table 后，scan pipeline 才能遍历 hash table 产出结果。

```
Q1 Physical Plan:
  Sort
   └── Projection
        └── HashAggregate
             └── TableScan(filter: l_shipdate <= ...)

Pipeline 0 (build): TableScan(filter) → HashAggregate.build()
Pipeline 1 (scan):  HashAggregate.scan() → Projection → Sort → ResultWriter
```

### 2.2 Operator Translator 组装

每个 pipeline 通过 produce/consume 协议组装 operator translator chain：

**Pipeline 0**:
```
TableScanTranslator.produce()
  → 生成循环，每行调用 parent_->consume()
    → HashAggregateTranslator.consume()
      → hash + lookup/insert + update aggregates
```

**Pipeline 1**:
```
HashAggregateTranslator.produceScan()
  → 遍历 hash table slots，每个 occupied entry 调用 parent_->consume()
    → ProjectionTranslator.consume()
      → 计算 AVG = SUM/COUNT 等，调用 parent_->consume()
        → SortTranslator.consume()
          → 写入 result buffer（sort 在 host 端执行）
```

### 2.3 Output Chain

Pipeline 1 的 output chain（Projection → Sort → ResultWriter）通过 `buildOutputChain()` 统一构建。这是一个可复用的组件：

```cpp
struct OutputChain {
    ResultWriterTranslator* resultWriter;  // 顶层，写 result buffer
    SortTranslator* sortTranslator;        // 可选
    ProjectionTranslator* projTranslator;  // 可选
    OperatorTranslator* bottom;            // 插入源 operator 的位置
};
```

---

## 3. Pipeline Function

### 3.1 签名

所有 pipeline function 使用统一签名：

```
(PTR, INT64, INT64) → VOID
  arg0: RuntimeContext*
  arg1: startRow (table row index 或 hash table slot index)
  arg2: endRow
```

在 IR 层表示为 `createFunction(name, VOID, {PTR, INT64, INT64})`。

### 3.2 Codegen 流程

```cpp
CodegenContext cg;
setActiveContext(&cg);  // 设置 thread-local context（operator overload 需要）

auto funcId = cg.createFunction("pipeline_build", VOID, {PTR, INT64, INT64});
CompilationContext ctx(cg);
ctx.runtimeCtx = cg.getArg<Ptr<Int8>>(0);  // arg0: RuntimeContext*

rootOperator->produce(ctx);  // 生成 pipeline body
cg.emitRetVoid();

auto program = cg.finalize();  // 注册 host functions，输出 IR Program
setActiveContext(nullptr);
```

### 3.3 Host Functions

Pipeline function 通过 host function 机制调用 C++ runtime：

| Host Function | 用途 | Pipeline |
|---------------|------|----------|
| `ht_lookup` | hash table 查找 | Build |
| `ht_insert` | hash table 插入 | Build |
| `ht_probe_next` | hash table 冲突链遍历 | Build |
| `ht_get_capacity` | 获取 slot 数量 | Scan |
| `ht_get_slot_buffer` | 获取 slot 数组指针 | Scan |
| `ht_get_slot_stride` | 获取 slot 字节步长 | Scan |

Host function 通过 `CodegenContext::registerHostFunction()` 注册，在 `finalize()` 时写入 `ir::Program`。

---

## 4. RuntimeContext

RuntimeContext 是 pipeline function 与 host 之间的唯一通信通道，以 C struct 形式传递：

```cpp
struct RuntimeContext {
    void** columnPtrs;         // offset 0:  列指针数组（指向 Table 列存储）
    uint64_t rowCount;         // offset 8:  总行数
    const char* stringBase;    // offset 16: 字符串池基地址
    void* hashTable;           // offset 24: hash table 指针
    void* resultBuffer;        // offset 32: 结果输出 buffer
    uint64_t resultRowCount;   // offset 40: 输出行数（由 pipeline 递增）
};
```

Pipeline function 内部通过硬编码 offset 访问这些字段（`GEP(rtCtx, offset)`）。

### 4.1 Build Pipeline 的 RuntimeContext

```
columnPtrs  → table.columnPtrs()
rowCount    → table.rowCount
stringBase  → table.stringPool()  (CHAR(n>1) 需要)
hashTable   → hashAggTranslator->hashTablePtr()
resultBuffer → null (不使用)
resultRowCount → 0 (不使用)
```

### 4.2 Scan Pipeline 的 RuntimeContext

```
columnPtrs  → table.columnPtrs()   (projection 可能引用原始列)
rowCount    → table.rowCount
hashTable   → hashAggTranslator->hashTablePtr() (同一 hash table)
resultBuffer → collector.buffer()   (预分配的 int64_t 数组)
resultRowCount → 0                   (pipeline 执行时递增)
```

---

## 5. 三阶段执行

每个 pipeline 经历 codegen → compile → execute 三个阶段：

```
             codegen          compile           execute
Pipeline 0:  [Tidy Tuples]  → [Backend JIT]   → [call func(ctx, 0, N)]
Pipeline 1:  [Tidy Tuples]  → [Backend JIT]   → [call func(ctx, 0, M)]
                                                      │
                                                      ▼
                                              ResultCollector.materialize()
                                                      │
                                                      ▼
                                              Host-side sort (if ORDER BY)
```

### 5.1 Profiling

每个阶段独立计时，存入 `PipelineProfile`：

```cpp
struct PipelineProfile {
    double codegenMs;   // Tidy Tuples codegen
    double compileMs;   // IR finalize + JIT compile
    double executeMs;   // pipeline function execution
};
```

通过 `enableProfiling()` 开启，EXPLAIN PROFILE 使用。

### 5.2 Backend Dispatch

Execution 层通过 `Backend` enum 选择后端，编译和执行分别 dispatch：

```
compile:
  INTERPRETER  → no-op (直接解释)
  FLYING_START → FlyingStartCompiler::compile()
  LLVM         → LLVMCompiler::compile()

execute:
  INTERPRETER  → Interpreter::execute(program, funcId, args)
  FLYING_START → FlyingStartCompiler::execute(funcPtr, args)
  LLVM         → LLVMCompiler::execute(funcPtr, args)
```

JIT compiler 实例必须存活到 execute 完成（它拥有 JIT'd code 的内存）。

---

## 6. Result Collection

### 6.1 ResultCollector

ResultCollector 管理 pipeline 输出：

1. **prepare(maxRows)**: 分配 `maxRows * numColumns` 的 `int64_t` buffer
2. **Pipeline 执行**: 生成代码写入 `buffer[row * numCols + col]`，递增 `RuntimeContext.resultRowCount`
3. **materialize(numRows)**: 从 buffer 转换为 `ResultRow` 对象
4. **sort()**: host 端排序（如果有 ORDER BY）

### 6.2 Host-Side Sort

Sort 在 host 端（C++ 代码）执行，而不是在生成代码中：

- SortTranslator 在 codegen 阶段只是透传 tuple 到 ResultWriter
- Pipeline 执行完成后，`ResultCollector::sort()` 对 materialized rows 做 `std::stable_sort`
- 对 Q1（~4 行结果），host-side sort 比生成排序代码更简单且足够快

### 6.3 Result Schema

Schema 从 physical plan 推导：
- 有 Projection：column names 来自 `projOp->aliases`，types 来自 expression result types
- 无 Projection：column names 为 `group_0, agg_0, ...`，types 从 agg operator 推导

---

## 7. Shared State

### 7.1 HashAggregateTranslator

`HashAggregateTranslator` 是跨 pipeline 的共享状态：

- Pipeline 0 创建并持有 translator 实例（含 `HashTable`）
- Pipeline 0 的 codegen 使用 `produce()`（build side）
- Pipeline 1 的 codegen 使用 `produceScan()`（scan side）
- 两个 pipeline 通过 `RuntimeContext.hashTable` 指向同一个 `HashTable` 实例

`PipelineExecutor` 持有 `hashAggTranslator_` 的 `unique_ptr`，生命周期跨越两个 pipeline。

### 7.2 Table

Table 在两个 pipeline 之间共享（只读），Pipeline 0 读列数据，Pipeline 1 可能通过 Projection 引用原始列。

---

## 8. EXPLAIN 集成

PipelineExecutor 提供 codegen-only 路径用于 EXPLAIN：

| EXPLAIN Level | 调用 | 输出 |
|---------------|------|------|
| EXPLAIN IR | `generateIR()` → `codegenAllPipelines()` | 每个 pipeline 的 IR 文本 |
| EXPLAIN ASM | `generateASM()` → codegen + compile（with log） | 每个 pipeline 的 x86/LLVM IR |
| EXPLAIN PROFILE | `execute()` with `enableProfiling()` | 每个 pipeline 的三阶段计时 |

`generateIR()` 和 `generateASM()` 完成后重置 `hashAggTranslator_`，避免 EXPLAIN 留下副作用。

---

## 9. 源码结构

| 文件 | 职责 |
|------|------|
| `src/execution/pipeline.h` | `PipelineExecutor`、`Backend` enum、`PipelineProfile`、`CodegenResult` |
| `src/execution/pipeline.cpp` | Pipeline 分解、codegen、compile、execute、operator 查找 |
| `src/execution/result_collector.h` | `ResultCollector`、`ResultColumn`、`ResultRow` |
| `src/execution/result_collector.cpp` | Buffer 管理、materialize、host-side sort |
| `src/storage/runtime_context.h` | `RuntimeContext` struct 定义 |
