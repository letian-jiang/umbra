# Tidy Tuples Design Document

## 1. Overview

Tidy Tuples 是代码生成框架，将物理查询计划在**单次遍历**中翻译为 Umbra IR 程序。它分为五个层次，自上而下每层提供更细粒度的抽象，最终归结为 IR 指令的 append。

```
物理查询计划
     │
     ▼
┌─────────────────────────────────────┐
│  Layer 1: Operator Translators      │  produce/consume 接口
│  Layer 2: Data Structures           │  HashTable 等算法组件
│  Layer 3: Tuples                    │  pack/unpack/hash
│  Layer 4: SQL Values                │  类型化运算 + NULL 语义
│  Layer 5: Codegen API               │  静态类型 IR 构建接口
└─────────────────────┬───────────────┘
                      │ emit
                      ▼
                  Umbra IR
```

### 设计原则

1. **单次遍历**：遍历物理计划树一次即生成完整 IR 程序，无多轮 pass
2. **层次分离**：上层处理粗粒度关系代数语义，下层处理细粒度机器原语；每层只用下一层的 API
3. **系统编译期安全**：类型检查在 C++ 编译期完成（Codegen API 的模板 + 重载），query 编译期零开销

---

## 2. Layer 5: Codegen API

最底层，直接操作 IR Builder（见 doc/ir.md §6），提供**静态类型**的 C++ 包装。

### 2.1 Primitive Types

对应 IR 的 TypeId，但在 C++ 层面是不同类型，借助重载保证类型安全：

| C++ Type    | IR TypeId | 语义 |
|-------------|-----------|------|
| `Bool`      | Bool      | 比较结果 / 条件 |
| `Int8`      | Int8      | 小整数 |
| `Int32`     | Int32     | 32-bit 整数 / DATE |
| `Int64`     | Int64     | 64-bit 整数 / DECIMAL |
| `UInt64`    | UInt64    | 无符号 / hash 值 |
| `Ptr<T>`    | Ptr       | 类型化指针（T 仅在 C++ 编译期存在）|

每个类型内部持有一个 `IROp`（4-byte instruction offset）。运算符重载直接 emit IR 指令：

```cpp
Int64 a = ...;
Int64 b = ...;
Int64 c = a + b;   // → builder.emitAdd(Int64, a.op, b.op)
Bool  d = a < b;   // → builder.emitCmpLt(Int64, a.op, b.op)
```

`Ptr<T>` 的 `load()` 和 `store()` 自动推导 T 的 IR TypeId：

```cpp
Ptr<Int64> p = ...;
Int64 v = p.load();     // → builder.emitLoad(Int64, p.op)
p.store(v);             // → builder.emitStore(v.op, p.op)
```

### 2.2 Control Flow

三个 RAII 类管理 basic block 和 PHI 节点的创建，将 SSA 构造隐藏在 C++ 作用域语义中：

**If**
```cpp
Bool cond = ...;
If test(cond);
    Int64 a = ...;         // then block
test.elseBlock();
    Int64 b = ...;         // else block
test.done();
Int64 result = test.phi(a, b);  // merge block, insert PHI
```

构造函数：创建 then/else/merge 三个 block（via `builder.createBlock()`），emit `condbr` 到 then/else。
`elseBlock()`：emit `br` 到 merge，切换当前 block 到 else。
`done()`：emit `br` 到 merge，切换到 merge block。
`phi(a, b)`：emit `phi2`，incoming = [(then_block, a), (else_block, b)]。

**Loop**
```cpp
Int64 init_i = codegen.const_(Int64, 0);
Ptr<Int8> init_ptr = ...;
Loop loop("scan", entryCondition, {init_i, init_ptr});
    Int64 i = loop.getLoopVar<Int64>(0);
    Ptr<Int8> ptr = loop.getLoopVar<Ptr<Int8>>(1);
    // loop body...
    Int64 next_i = i + codegen.const_(Int64, 1);
    Ptr<Int8> next_ptr = ...;
loop.done(exitCondition, {next_i, next_ptr});
```

构造函数：创建 header/body/exit 三个 block。在 header 中 emit PHI（初始值来自 pre-header）。检查 entry condition，`condbr` 到 body 或 exit。
`getLoopVar<T>(i)`：返回第 i 个 PHI 的值，类型为 T。
`done()`：emit `br` 回 header，backpatch PHI 的 back-edge 值（见 doc/ir.md §9.1）。切换到 exit block。

**Function**
```cpp
auto fn = codegen.createFunction("pipeline_0", Void, {Ptr<Int8>});
Ptr<Int8> tableBase = fn.getArg<Ptr<Int8>>(0);
// ... function body ...
codegen.emitRetVoid();
```

### 2.3 Host Function Calls

调用预编译的 C++ 函数（如 hash table 操作、内存分配）：

```cpp
// 在 system compile time 注册 host function
HostFunction htInsert = codegen.registerHostFunction(
    "ht_insert", reinterpret_cast<void*>(&HashTable::insert),
    Ptr<Int8>,                // return type
    {Ptr<Int8>, UInt64}       // param types: table ptr, hash
);

// 在 query compile time 生成调用
Ptr<Int8> entry = codegen.call(htInsert, tablePtr, hashVal);
```

> **[MVP simplification]** 论文的 proxy 系统在系统编译期自动为 C++ 类生成代理，可从生成代码中访问成员和调用方法，并支持 inline 标记。MVP 简化为手动注册函数指针 + `call` 指令，不做自动代理生成。

---

## 3. Layer 4: SQL Values

封装 SQL 类型语义：类型化运算、NULL 传播、隐式 cast。这一层是泛型代码（SQL 类型在运行时确定）和静态类型代码（Codegen API 在编译期确定）之间的桥梁。

### 3.1 SQLValue

```
SQLValue:
    IROp value       // 实际值的 IROp
    IROp null_flag   // Bool 类型的 IROp（1 = is null）
    SQLType type     // SQL 类型元信息（type_id, precision, scale 等）
```

`value` 和 `null_flag` 都是 IROp——它们不是运行时的 C++ 值，而是 IR 程序中的 SSA 值。SQLValue 的操作不执行运算，而是 **emit IR 指令**来执行运算。

### 3.2 Binary Operations

```cpp
SQLValue SQLValue::evaluateBinary(BinaryOp op, SQLValue lhs, SQLValue rhs) {
    // 1. 推导结果类型
    SQLType resultType = inferBinaryResultType(op, lhs.type, rhs.type);

    // 2. 如果需要，插入 implicit cast
    lhs = castIfNeeded(lhs, commonType);
    rhs = castIfNeeded(rhs, commonType);

    // 3. NULL 传播：if either is null, result is null
    IROp resultNull = codegen.lor(lhs.null_flag, rhs.null_flag);

    // 4. 根据底层 IR 类型分派到 Codegen API
    IROp resultValue;
    switch (resultType.toIRType()) {
        case Int64:
            Int64 a(lhs.value), b(rhs.value);
            if (op == ADD) resultValue = (a + b).op;
            if (op == MUL) resultValue = (a * b).op;
            // ...
    }

    // 5. DECIMAL 特殊处理：乘法后重新定标
    if (resultType.isDecimal() && op == MUL) {
        Int64 v(resultValue);
        Int64 scale = codegen.const_(Int64, resultType.scaleFactor());
        resultValue = (v / scale).op;
    }

    return SQLValue{resultValue, resultNull, resultType};
}
```

### 3.3 Comparison

比较返回 `SQLValue`（类型为 BOOLEAN）：

```cpp
SQLValue SQLValue::evaluateCompare(CmpOp op, SQLValue lhs, SQLValue rhs) {
    // cast + null propagation 同上
    IROp cmpResult;
    switch (op) {
        case LT: cmpResult = codegen.emitCmpLt(lhs.irType(), lhs.value, rhs.value);
        case LE: cmpResult = codegen.emitCmpLe(...);
        // ...
    }
    return SQLValue{cmpResult, resultNull, SQLType::BOOLEAN};
}
```

### 3.4 Q1 所需运算

| 运算 | 场景 | 处理 |
|------|------|------|
| DATE comparison | `l_shipdate <= date_threshold` | Int32 比较 |
| DECIMAL * DECIMAL | `l_extendedprice * (1 - l_discount)` | Int64 乘法 + 重新定标 |
| DECIMAL + DECIMAL | `SUM(...)` 累加 | Int64 加法（同 scale 直接加）|
| DECIMAL / BIGINT | `AVG = SUM / COUNT` | Int64 除法 + scale 调整 |
| Integer increment | `COUNT(*)` | Int64 加 1 |

> **[MVP simplification]** 论文中 SQLValue 还支持 substring search、LIKE 匹配、overflow checking（通过 checked arithmetic）等丰富操作。MVP 只实现 Q1 所需子集。

---

## 4. Layer 3: Tuples

操作多个 SQLValue 的集合——序列化、反序列化、hash。

### 4.1 Layout Computation

给定一组 SQLType，计算序列化后的内存布局：

```
TupleLayout:
    compute(types: SQLType[]) → Layout
        // 为每个 type 分配 offset（考虑对齐）
        // null bitmap 放在最后

Layout:
    total_size: uint32_t
    field_offsets: uint32_t[]    // 每个字段相对于 tuple 起始地址的偏移
    null_offset: uint32_t       // null bitmap 的偏移
```

### 4.2 Pack

将多个 SQLValue 写入一块内存区域（如 hash table entry）：

```cpp
void Tuples::pack(Ptr<Int8> target, vector<SQLValue>& values, Layout& layout) {
    for (int i = 0; i < values.size(); i++) {
        Ptr<Int8> fieldPtr = target + codegen.const_(Int32, layout.field_offsets[i]);
        // cast fieldPtr to Ptr<FieldType>, then store
        auto typed = fieldPtr.bitcastTo<FieldType>();
        typed.store(values[i].value);
    }
    // pack null flags into bitmap
    packNullBitmap(target + layout.null_offset, values);
}
```

### 4.3 Unpack

从内存区域读出 SQLValue 序列：

```cpp
vector<SQLValue> Tuples::unpack(Ptr<Int8> source, Layout& layout, vector<SQLType>& types) {
    vector<SQLValue> result;
    for (int i = 0; i < types.size(); i++) {
        Ptr<Int8> fieldPtr = source + codegen.const_(Int32, layout.field_offsets[i]);
        IROp value = fieldPtr.bitcastTo<FieldType>().load();
        IROp null = unpackNullBit(source + layout.null_offset, i);
        result.push_back(SQLValue{value, null, types[i]});
    }
    return result;
}
```

### 4.4 Hash

计算多个 SQLValue 的 hash，用于 hash aggregation 的 key：

```cpp
UInt64 Tuples::hash(vector<SQLValue>& keys) {
    // 论文 §2.5: 使用两路 CRC32
    UInt64 h1 = codegen.const_(UInt64, HASH_SEED_1);
    UInt64 h2 = codegen.const_(UInt64, HASH_SEED_2);
    for (auto& key : keys) {
        UInt64 v = zextOrTruncTo64(key.value);
        h1 = codegen.crc32(v, h1);
        h2 = codegen.crc32(v, h2);
    }
    // 组合两路: hash = (h1 ^ rotr(h2, 32)) * prime
    UInt64 h2_rot = codegen.rotr(h2, 32);
    UInt64 combined = h1 ^ h2_rot;
    return combined * codegen.const_(UInt64, MIX_PRIME);
}
```

> **[MVP simplification]** 论文使用 `Data128` 类型将两路 CRC32 拼接成 128-bit 中间值再组合。MVP 去掉了 `Data128` 类型，改用两次独立的 CRC32 + 位运算组合。hash 质量足够用于 Q1 的 4 个 group。

**Implementation**: `src/codegen/tuples/tuple_layout.h/.cpp`. `TupleLayout` 构造时计算 field offsets 和 null bitmap 位置。`pack()`/`unpack()` 使用 GEP + bitcast + store/load emit IR。null bitmap 使用 bit-level shift/or/and 操作逐 bit 打包。`hash()` 是静态方法，对小于 64-bit 的类型使用 `emitZExt`，对 64-bit 类型使用 `emitBitcast`。

---

## 5. Layer 2: Data Structures

提供算法级别的组件，内部调用 Layer 3/4/5 来 emit 代码。Q1 的核心组件是 **HashTable**。

### 5.1 HashTable

**运行时数据结构**（预编译的 C++ 代码）：
- 开放寻址，线性探测
- 每个 slot: `[occupied(1B) | hash(8B) | payload(variable)]`
- `insert(hash, size) → Ptr<Int8>`：找到空 slot，返回 payload 指针
- `lookup(hash) → Ptr<Int8>`：找到匹配 slot，返回 payload 指针
- `iterate(callback)`：遍历所有 occupied slot

**代码生成接口**（emit IR 指令）：

```cpp
class HashTableCodegen {
    Layout keyLayout;
    Layout payloadLayout;  // group-by keys + aggregate states

    // 由 HashAggregateOperator 调用
    void buildLayout(vector<SQLType>& keys, vector<SQLType>& aggStates);

    // emit: 查找或插入 entry，返回 entry 指针
    Ptr<Int8> findOrInsert(Ptr<Int8> tablePtr, UInt64 hash,
                           vector<SQLValue>& keys);

    // emit: 遍历所有 entry，对每个调用 callback
    void scan(Ptr<Int8> tablePtr, function<void(Ptr<Int8>)> callback);
};
```

`findOrInsert` 生成的代码模式：

```
  1. call host function: entry_ptr = ht_lookup(table, hash)
  2. if entry_ptr != null:
  3.     unpack keys from entry_ptr
  4.     compare with input keys (逐字段)
  5.     if match: return entry_ptr        // 已有 group
  6.     // else: linear probe (call host function for next slot)
  7. else:
  8.     call host function: entry_ptr = ht_insert(table, hash, size)
  9.     pack keys into entry_ptr
  10.    initialize aggregate states to zero
  11.    return entry_ptr                   // 新 group
```

其中步骤 3-5, 9-10 是生成的 IR 代码（使用 Layer 3 的 pack/unpack 和 Layer 4 的 compare），步骤 1, 6, 8 是对预编译 C++ host function 的 `call`。

> **[MVP simplification]** 论文的 hash table 实现完全在生成代码中（包括 probing loop）。MVP 将 probing 逻辑放在预编译的 host function 中，只生成 key comparison 和 pack/unpack 的代码。这简化了代码生成，但增加了 call 开销。对 Q1 的 ~4 个 group 影响可忽略。

**Implementation**: `src/codegen/structures/hash_table.h/.cpp`. `HashTable` 运行时类实现 open addressing with linear probing，slot 布局 `[occupied(1B)|hash(8B)|payload]`，load factor 70% 自动 resize。`HashTableCodegen` 的 `findOrInsert()` 生成三层结构：fast path（lookup 返回 null 直接 insert）、first-match check（比较第一个候选的 keys）、probe loop（loop + IfBuilder + probeNext host call）。`scan()` 通过 host function 获取 capacity/slotBuffer/stride，emit 一个 slot 遍历 loop，检查 occupied flag，对有效 slot 回调 callback。

### 5.2 Aggregate State

aggregate state 和 group-by keys 一起存储在 hash table entry 的 payload 中：

```
Entry layout (Q1):
┌──────────┬──────────────┬──────────────────────────────────────┐
│ hash(8B) │ keys         │ aggregate states                     │
│          │ returnflag(1)│ sum_qty(8) sum_base_price(8)          │
│          │ linestatus(1)│ sum_disc_price(8) sum_charge(8)       │
│          │ pad(6)       │ sum_discount(8) count(8)              │
└──────────┴──────────────┴──────────────────────────────────────┘
                           total payload ≈ 64 bytes
```

---

## 6. Layer 1: Operator Translators

每个物理算子实现 **produce/consume** 接口。整个 pipeline 的代码生成由 produce 调用链驱动，consume 在子算子产出 tuple 时被回调。

### 6.1 Produce/Consume Protocol

```
Pipeline compilation:
    pipeline.rootOperator.produce(context)

Operator::produce(context):
    // 准备工作（如创建 hash table）
    childOperator.produce(context)    // 递归驱动子算子
    // 收尾工作

Operator::consume(context, tuple):
    // 处理从子算子传来的 tuple
    // 然后调用 parentOperator.consume(context, outputTuple)
```

### 6.2 TableScanOperator

```
produce(context):
    Ptr<Int8> colPtrs[N]    // 各列的基地址（从 CompilationContext 获取）
    Int64 rowCount = ...    // 行数
    Int64 zero = const(0)

    Loop loop("scan", i < rowCount, {zero}):
        Int64 i = loop.getLoopVar(0)

        // 加载 projection pushdown 后所需的列
        for each needed column col:
            Ptr<T> colBase = colPtrs[col.index]
            Ptr<T> elemPtr = colBase + i       // getelementptr
            SQLValue val = SQLValue{elemPtr.load(), null_flag, col.type}
            tuple.push_back(val)

        // filter (predicate pushdown 后集成在 scan 中)
        if (hasFilter):
            SQLValue filterResult = evaluateFilterExpr(tuple)
            If test(filterResult.value)
                parent.consume(context, tuple)   // 只有通过 filter 的 tuple 继续
            test.done()
        else:
            parent.consume(context, tuple)

        Int64 next_i = i + const(1)
    loop.done(next_i < rowCount, {next_i})
```

### 6.3 HashAggregateOperator

**Pipeline 1 (build side):**

```
produce(context):
    // 创建运行时 hash table（预编译 C++ 对象）
    // 将 table 指针作为常量嵌入生成代码
    child.produce(context)    // 驱动 scan

consume(context, tuple):
    // 从 tuple 中提取 group-by keys
    vector<SQLValue> keys = extractKeys(tuple)

    // 计算 hash
    UInt64 hash = Tuples::hash(keys)

    // 查找或插入 entry
    Ptr<Int8> entry = hashTable.findOrInsert(tablePtr, hash, keys)

    // 更新 aggregate states
    Ptr<Int8> statePtr = entry + const(payloadLayout.stateOffset)
    for each aggregate agg:
        Ptr<T> slotPtr = (statePtr + agg.offset).bitcastTo<Ptr<T>>()
        T oldVal = slotPtr.load()
        SQLValue inputVal = evaluateAggExpr(agg, tuple)
        T newVal = computeAggUpdate(agg.func, oldVal, inputVal)
        slotPtr.store(newVal)
```

其中 `computeAggUpdate`：

| Aggregate | Update 逻辑 |
|-----------|------------|
| `SUM(x)`  | `old + x` |
| `COUNT(*)` | `old + 1` |
| `AVG(x)` | 拆分为 `SUM(x)` + `COUNT(*)`，Pipeline 2 再做除法 |

**Pipeline 2 (scan side):**

```
produce(context):
    // 遍历 hash table 的所有 entry
    hashTable.scan(tablePtr, [&](Ptr<Int8> entry) {
        // unpack group-by keys
        vector<SQLValue> keys = Tuples::unpack(entry + keyOffset, keyLayout, keyTypes)

        // unpack aggregate states
        // finalize: AVG = SUM / COUNT
        vector<SQLValue> results = finalizeAggregates(entry + stateOffset)

        // 组合输出 tuple
        vector<SQLValue> outputTuple = concat(keys, results)
        parent.consume(context, outputTuple)
    })
```

### 6.4 ResultWriterTranslator

Pipeline 的根节点（sink）。接收 tuple，将每个 SQLValue sign-extend 到 Int64 后写入结果缓冲区。

**Implementation**: `src/codegen/operators/result_writer.h/.cpp`.

```
consume(context, tuple):
    Ptr<Int64> bufPtr = load(RuntimeContext.resultBuffer)
    Int64 rowCount = load(RuntimeContext.resultRowCount)
    for each SQLValue val in tuple:
        Int64 asI64 = sext_or_bitcast(val.value)
        buf[rowCount * numCols + colIdx].store(asI64)
    store(RuntimeContext.resultRowCount, rowCount + 1)
```

### 6.5 SortTranslator

**Implementation**: `src/codegen/operators/sort.h/.cpp`.

For Q1, the sort is a codegen passthrough. Sorting is done post-pipeline by the `ResultCollector` on the host side, since the result set is tiny (~4 rows). The `SortTranslator` simply delegates `produce()` to its child and `consume()` to its parent.

### 6.6 ProjectionTranslator

**Implementation**: `src/codegen/operators/projection.h/.cpp`.

Evaluates projection expressions over the input tuple from the hash aggregate scan. Expressions reference group-by keys via `BoundGroupRef` and aggregate results via `BoundAggRef`. Supports `BinaryExpr`, `Literal`, `Cast` in projection expressions.

The number of group-by keys is inferred at codegen time by scanning the input tuple for the boundary between non-BIGINT group keys and BIGINT aggregate states.

### 6.7 Q1 的完整 Pipeline 调用链

**Pipeline 1:**
```
ResultOperator.produce()
  → HashAggregateOperator.produce()       // 创建 hash table
    → TableScanOperator.produce()          // emit scan loop
        Loop(0..rowCount):
          load 7 columns
          evaluate filter: l_shipdate <= threshold
          If(passes):
            → HashAggregateOperator.consume()
              hash(returnflag, linestatus)
              findOrInsert → entry
              entry.sum_qty += l_quantity
              entry.sum_base_price += l_extendedprice
              disc = l_extendedprice * (1 - l_discount)
              entry.sum_disc_price += disc
              entry.sum_charge += disc * (1 + l_tax)
              entry.sum_discount += l_discount
              entry.count += 1
```

**Pipeline 2:**
```
ResultOperator.produce()
  → HashAggregateOperator.produce()       // scan hash table
    scan(tablePtr):
      for each entry:
        unpack keys
        avg_qty = sum_qty / count
        avg_price = sum_base_price / count
        avg_disc = sum_discount / count
        → ResultOperator.consume()
          write to result buffer
```

---

## 7. CompilationContext

**Implementation**: `src/codegen/operators/operator_translator.h` (struct `CompilationContext`).

Operator translator 之间共享的编译期上下文：

```
CompilationContext:
    CodegenContext& codegen     // Layer 5 typed IR builder
    Ptr<Int8> runtimeCtx       // 运行时上下文指针（JIT 函数参数）
```

每个 pipeline 编译为一个 IR function，其签名为 `void pipeline_N(RuntimeContext* ctx)`。RuntimeContext 包含 column pointers, row count, hash table pointer, result buffer pointer, 和 result row count。

> **[Implementation note]** 原设计包含 `Catalog& catalog` 和 `Pipeline& currentPipeline`。实际实现中，catalog 信息通过 `PhysicalOperator` 节点传递（如 `PhysicalTableScan.table`），pipeline 结构由 `PipelineExecutor` 管理，因此 `CompilationContext` 简化为只持有 `CodegenContext` 和 `runtimeCtx`。

---

## 8. Pipeline Execution

**Implementation**: `src/execution/pipeline.h/.cpp` (`PipelineExecutor`), `src/execution/result_collector.h/.cpp` (`ResultCollector`).

```
1. PipelineExecutor.execute(OptimizeResult, Table, ResultCollector):
   a. executeBuildPipeline:
      - Create TableScanTranslator + HashAggregateTranslator
      - Wire produce/consume chain
      - Generate IR via CodegenContext
      - Finalize → IR Program
      - Execute via Interpreter, passing RuntimeContext
   b. executeScanPipeline:
      - Create ProjectionTranslator + SortTranslator + ResultWriterTranslator
      - HashAggregateTranslator.produceScan() drives the chain
      - Generate IR, finalize, execute via Interpreter
      - Materialize results from buffer → ResultCollector
      - Host-side sort on materialized rows

2. Pipeline 间通过 RuntimeContext 中的 HashTable 指针通信
   (Pipeline 0 写入 hash table → Pipeline 1 读取 hash table)

3. ResultCollector 收集输出行，支持 host-side 排序
```

> **[Implementation note]** `CodegenContext.registerHostFunction` 调用 `IRBuilder.createFunction` 会改变当前插入点。实现中在注册前后保存/恢复插入点，确保 host function 注册不影响正在生成的 pipeline 函数代码。

---

## 9. MVP Simplifications Summary

| 论文特性 | MVP 处理 | 影响 |
|---------|---------|------|
| Proxy 系统（自动生成 C++ 函数代理）| 手动注册函数指针 + call | codegen 代码稍多 |
| Hash table probing 在生成代码中 | Probing 在预编译 host function 中 | 多一次 call 开销，Q1 可忽略 |
| 完整 SQLValue 运算集 | 只实现 Q1 所需子集 | 不支持 LIKE/substring 等 |
| Sort operator translator | 结果集极小（4 行），host function 排序 | 不影响性能 |
| 多种 aggregate 函数 | 只实现 SUM/COUNT/AVG | AVG = SUM/COUNT |
| Join operator translator | 不实现 | Q1 无 join |
| Subquery flattening | 不实现 | Q1 无 subquery |
