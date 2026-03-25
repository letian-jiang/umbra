# Testing Design Document

## 1. Overview

测试策略围绕一个核心思路：**两个独立实现互相验证**。

- IR 层：Interpreter vs Flying Start 必须对任何 IR 程序产生相同结果
- SQL 层：对 **MVP 支持的 SQL 子集**（single-table SELECT + WHERE + GROUP BY + ORDER BY + supported aggregates），我们的系统 vs DuckDB（differential oracle）必须产生相同结果集

```
                    ┌──────────────────────────┐
                    │       SQL Tests          │
                    │  our system vs DuckDB    │
                    └────────────┬─────────────┘
                                 │
          ┌──────────────────────┼───────────────────────┐
          │                      │                       │
   ┌──────▼──────┐    ┌─────────▼─────────┐   ┌────────▼────────┐
   │ Unit Tests  │    │    IR Tests        │   │  Perf Tests     │
   │ per module  │    │ Interp vs Flying   │   │  latency check  │
   └─────────────┘    └───────────────────┘   └─────────────────┘
```

### 框架

**Catch2** for all tests. 选型理由：header-only 可选、宏简洁、支持 `SECTION` 做参数化、与 CMake/CTest 集成好。

---

## 2. Unit Tests

每个模块的内部逻辑独立测试，不依赖其他模块。

### 2.1 Storage

| 测试 | 内容 |
|------|------|
| `decimal_parse` | `"21168.23"` + scale=2 → `2116823` |
| `decimal_edge` | `"0.00"`, `"-1.50"`, `"99999999999999.99"` |
| `date_parse` | `"1998-09-02"` → 正确的 epoch 偏移值 |
| `date_epoch` | `"2000-01-01"` → `0` |
| `date_negative` | `"1998-01-01"` → 负数 |
| `tbl_load` | 加载 lineitem.tbl 前几行，验证每列的值和 row_count |
| `varchar_load` | 加载后 StringValue.offset + string_buffer.data() 指向正确字符，len 正确 |

### 2.2 Parser

| 测试 | 内容 |
|------|------|
| `simple_select` | `SELECT 1` → AST 结构验证 |
| `column_ref` | `SELECT a FROM t` → ColumnRef("a") |
| `binary_expr` | `SELECT a + b * c FROM t` → 运算符优先级正确 |
| `func_call` | `SUM(x)`, `COUNT(*)`, `AVG(x)` → FuncCall 节点 |
| `date_interval` | `DATE '1998-12-01' - INTERVAL '90' DAY` → DateArith 节点 |
| `q1_full` | 完整 Q1 SQL → AST 结构与预期匹配 |
| `syntax_error` | `SELECT FROM` → 返回错误，不崩溃 |
| `unsupported` | `SELECT * FROM t1 JOIN t2 ON ...` → 报 unsupported |

### 2.3 Binder

| 测试 | 内容 |
|------|------|
| `name_resolution` | `l_shipdate` → (table=0, col=10, type=DATE) |
| `unknown_column` | `l_unknown` → 报错 |
| `unknown_table` | `FROM nonexist` → 报错 |
| `type_inference_add` | DECIMAL + DECIMAL → DECIMAL（正确 scale）|
| `type_inference_mul` | DECIMAL * DECIMAL → DECIMAL（scale 相加）|
| `implicit_cast` | `1 - l_discount` → 1 被 cast 为 DECIMAL |
| `date_fold` | `DATE '1998-12-01' - INTERVAL '90' DAY` → DATE literal |
| `avg_decompose` | `AVG(x)` → SUM(x) + COUNT(*)，且与已有 SUM(x) 去重 |
| `agg_validation` | 非 agg 表达式不在 GROUP BY 中 → 报错 |
| `q1_plan` | 完整 Q1 → 验证 LogicalPlan 树结构 |

### 2.4 Optimizer

| 测试 | 内容 |
|------|------|
| `predicate_pushdown` | Filter 在 Projection 上方 → pushdown 到 Projection 下方 |
| `filter_into_scan` | LogicalFilter + LogicalScan → PhysicalTableScan(filter) |
| `projection_pushdown` | Q1 → scan 只含 7 列（4,5,6,7,8,9,10）|
| `pipeline_split` | Q1 physical plan → 2 个 pipeline |

### 2.5 IR Builder

| 测试 | 内容 |
|------|------|
| `const_create` | emit const i32 42 → 正确编码在 instruction buffer |
| `const_dedup` | 相同 const 两次 → 返回相同 IROp |
| `const_fold_add` | const(3) + const(4) → const(7) |
| `const_fold_mul_zero` | x * const(0) → const(0) |
| `const_fold_mul_one` | x * const(1) → x |
| `const_fold_branch` | condbr(const(true), A, B) → br A |
| `block_creation` | createBlock → 正确分配 block_id |
| `backpatch` | finalize 后 branch target 是正确的 byte offset |
| `phi_construction` | If + phi → 正确的 phi2 指令 |
| `loop_backpatch` | Loop 的 PHI back-edge 在 done() 后被正确回填 |
| `dce` | 未使用的指令被 DCE 移除 |

### 2.6 Codegen API (Layer 5)

| 测试 | 内容 |
|------|------|
| `type_safety` | `Int32 + Int64` → 编译期报错（C++ 类型不匹配）|
| `ptr_load_store` | `Ptr<Int64>` load/store → 正确的 IR load/store 指令 |
| `if_construct` | If/elseBlock/done → 正确的 block 结构和 condbr |
| `loop_construct` | Loop/done → header 中有 PHI，back-edge 正确 |
| `host_call` | call host function → 正确的 callN 指令 |

### 2.7 Hash Table

| 测试 | 内容 |
|------|------|
| `insert_lookup` | 插入 key → lookup 返回相同 entry |
| `insert_new` | 新 key → 返回新 entry，initialized to zero |
| `collision` | 不同 key 但相同 hash slot → 线性探测后正确区分 |
| `iterate` | 插入 N 个 entry → iterate 返回 N 个（顺序不要求）|
| `grow` | 插入超过 load factor → resize 后所有 entry 仍可 lookup |

---

## 3. IR Tests

手动构造 IR 程序，在 Interpreter 和 Flying Start 上分别执行，断言结果相同。

### 3.1 策略

```cpp
void runIRTest(Program& program, FunctionId func, vector<uint64_t> args,
               uint64_t expected) {
    // Interpreter
    uint64_t interpResult = interpreter.execute(program, func, args);
    REQUIRE(interpResult == expected);

    // Flying Start
    auto funcPtr = flyingStart.compile(program, func);
    uint64_t jitResult = funcPtr(args...);
    REQUIRE(jitResult == expected);

    // 两者一致
    REQUIRE(interpResult == jitResult);
}
```

### 3.2 Test Cases

**算术**:

| 测试 | IR | Expected |
|------|----|----------|
| `add_i64` | `ret i64 add(funarg(0), funarg(1))` | a + b |
| `sub_i64` | `ret i64 sub(funarg(0), funarg(1))` | a - b |
| `mul_i64` | `ret i64 mul(funarg(0), funarg(1))` | a * b |
| `div_i64` | `ret i64 div(funarg(0), funarg(1))` | a / b |
| `div_by_zero` | `div(a, 0)` | 定义行为（trap 或返回 0）|
| `neg_i64` | `ret i64 neg(funarg(0))` | -a |
| `chained` | `ret i64 add(mul(a,b), sub(c,d))` | a*b + c-d |

**比较 + 控制流**:

| 测试 | IR | Expected |
|------|----|----------|
| `cmp_branch` | `if a < b return 1 else return 0` | 正确的分支选择 |
| `cmp_eq` | `cmpeq(5, 5)` → true | 1 |
| `cmp_le` | `cmple(3, 5)` → true, `cmple(5, 3)` → false | |

**Memory**:

| 测试 | IR | Expected |
|------|----|----------|
| `load_store` | store 42 to ptr, load from ptr → 42 | |
| `gep_load` | GEP into array, load element | 正确的数组访问 |
| `gep_chain` | 连续 GEP 折叠 | 结果与未折叠一致 |

**循环**:

| 测试 | IR | Expected |
|------|----|----------|
| `sum_1_to_n` | loop i=1..n, acc += i → n*(n+1)/2 | |
| `nested_loop` | 双重循环 → 正确的迭代次数 | |

**函数调用**:

| 测试 | IR | Expected |
|------|----|----------|
| `call_host` | call C++ abs() → 正确结果 | |
| `call_multiarg` | call host func(a,b,c) → 正确传参 | |

**PHI**:

| 测试 | IR | Expected |
|------|----|----------|
| `if_phi` | `phi(then_val, else_val)` → 根据条件选择 | |
| `loop_phi` | loop counter → 正确递增 | |

**CRC32**:

| 测试 | IR | Expected |
|------|----|----------|
| `crc32_basic` | `crc32(data, seed)` → Interpreter 和 Flying Start 结果一致 | |

### 3.3 Flying Start 优化正确性

针对 Flying Start 的 4 项优化，专门构造触发特定优化的 IR 程序：

| 优化 | 测试 IR | 验证点 |
|------|---------|--------|
| Stack reuse | 多个短生命值 → 结果正确 | slot 被复用但不覆盖活跃值 |
| Register alloc | 深嵌套循环中的值 → 结果正确 | 寄存器溢出/恢复正确 |
| Lazy address | GEP chain + load → 结果正确 | 折叠后的地址计算正确 |
| Fuse cmp+branch | cmple 后接 condbr → 分支正确 | flags 未被中间指令破坏 |

---

## 4. SQL Tests (Differential Testing)

用 DuckDB 作为 oracle，对比两个系统的查询结果。

### 4.1 架构

```cpp
void runSQLTest(const string& sql) {
    // 1. 在 DuckDB 上执行
    auto duckResult = duckdb.query(sql);

    // 2. 在我们的系统上执行
    auto ourResult = umbra.query(sql);

    // 3. 比较结果集
    REQUIRE(resultSetsEqual(duckResult, ourResult, tolerance));
}
```

### 4.2 结果比较

```
resultSetsEqual(expected, actual, tolerance):
    1. 行数必须相等
    2. 列数必须相等
    3. 对每行每列：
       - INTEGER/BIGINT: 精确相等
       - DECIMAL: |expected - actual| <= 10^(-scale) （允许最后一位 rounding 差异）
       - DATE: 精确相等
       - VARCHAR: 精确相等
    4. 行的顺序：如果 SQL 有 ORDER BY，按序比较；否则先排序再比较
```

### 4.3 Test Cases

**Q1 完整测试**:

```sql
-- 标准 Q1 (INTERVAL 90 DAY)
SELECT l_returnflag, l_linestatus, SUM(l_quantity) AS sum_qty, ...
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

**Q1 参数变体** (TPC-H spec 允许 INTERVAL 60~120 DAY):

```sql
-- 不同 interval 值
... WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '60' DAY ...
... WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '120' DAY ...

-- 极端值: 所有行通过
... WHERE l_shipdate <= DATE '2999-12-31' ...

-- 极端值: 没有行通过
... WHERE l_shipdate <= DATE '1900-01-01' ...
```

**子表达式测试**（隔离验证各个 SQL 功能）:

| 测试 | SQL | 验证点 |
|------|-----|--------|
| `scan_only` | `SELECT l_orderkey FROM lineitem LIMIT 10` | 基础 scan |
| `filter_only` | `SELECT COUNT(*) FROM lineitem WHERE l_shipdate <= DATE '1998-09-02'` | filter 正确性 |
| `agg_sum` | `SELECT SUM(l_quantity) FROM lineitem` | SUM 无 group-by |
| `agg_count` | `SELECT COUNT(*) FROM lineitem` | COUNT(*) |
| `agg_group` | `SELECT l_returnflag, COUNT(*) FROM lineitem GROUP BY l_returnflag` | group-by |
| `expr_arith` | `SELECT l_extendedprice * (1 - l_discount) FROM lineitem LIMIT 5` | DECIMAL 乘法 |
| `order_by` | `SELECT l_returnflag FROM lineitem GROUP BY l_returnflag ORDER BY l_returnflag` | 排序 |

---

## 5. Performance Tests

不做功能断言，只测量延迟和吞吐。

### 5.1 Benchmark Harness

```cpp
void benchmarkQ1() {
    // warmup
    for (int i = 0; i < 3; i++)
        umbra.query(Q1_SQL);

    // measure
    auto times = vector<double>();
    for (int i = 0; i < 100; i++) {
        auto start = high_resolution_clock::now();
        umbra.query(Q1_SQL);
        auto end = high_resolution_clock::now();
        times.push_back(duration_ms(end - start));
    }

    // report
    report("Q1 SF=0.01", times);  // min, median, p95, mean
}
```

### 5.2 Metrics

| Metric | 测量方式 | 目标 (SF=0.01) |
|--------|---------|----------------|
| Total latency | end-to-end 包含 parse/bind/optimize/codegen/execute | < 3.1 ms |
| Plan time | parse + bind + optimize | < 0.3 ms |
| Codegen time | Tidy Tuples IR 生成 | < 0.3 ms |
| Compile time | Flying Start IR → x86 | < 0.3 ms |
| Execution time | pipeline 函数运行 | < 2.5 ms |

### 5.3 Phase Breakdown

每个阶段独立计时，输出类似论文 Table 2 的分解：

```
Q1 SF=0.01 (single-threaded):
  plan:    0.25 ms
  codegen: 0.20 ms
  x86:     0.18 ms
  exec:    2.10 ms
  total:   2.73 ms
```

### 5.4 Regression Detection

CI 中不设硬阈值（避免因机器差异 flaky），但记录每次 commit 的 latency 数据。手动 review 时对比趋势。

---

## 6. Test Data

### 6.1 TPC-H SF=0.01

默认测试数据。在 test fixture 中一次性加载，所有测试共享只读数据。

```cpp
class TPCHFixture {
    static Table lineitem;  // 进程级别共享

    static void SetUpTestSuite() {
        lineitem = loadTable("lineitem.tbl");
    }
};
```

### 6.2 Minimal Data

部分 unit test 不需要完整的 TPC-H 数据，使用手工构造的小数据集：

```cpp
Table makeTestTable() {
    // 3 rows, 2 columns
    auto t = Table("test");
    t.addColumn<int32_t>("id", {1, 2, 3});
    t.addColumn<int64_t>("value", {100, 200, 300});
    return t;
}
```

---

## 7. Test Organization

```
test/
├── unit/
│   ├── test_storage.cpp        # §2.1
│   ├── test_parser.cpp         # §2.2
│   ├── test_binder.cpp         # §2.3
│   ├── test_optimizer.cpp      # §2.4
│   ├── test_ir_builder.cpp     # §2.5
│   ├── test_codegen_api.cpp    # §2.6
│   └── test_hash_table.cpp     # §2.7
├── ir/
│   ├── test_arithmetic.cpp     # §3.2 算术
│   ├── test_control_flow.cpp   # §3.2 比较+分支
│   ├── test_memory.cpp         # §3.2 load/store/gep
│   ├── test_loop.cpp           # §3.2 循环
│   ├── test_call.cpp           # §3.2 函数调用
│   └── test_optimizations.cpp  # §3.3 Flying Start 优化正确性
├── sql/
│   ├── test_q1.cpp             # §4.3 Q1 完整 e2e execution + result verification
│   └── test_q1_variants.cpp    # §4.3 INTERVAL variants (60/90/120/180 DAY)
└── perf/
    └── test_benchmark.cpp      # §5 Q1 latency measurement + phase breakdown
```

---

## 8. CI Pipeline

```
Build → Unit Tests → IR Tests → SQL Tests → Perf Benchmark (report only)
                                                │
                                          不阻塞 CI，
                                          只记录数据
```

Unit / IR / SQL tests 中任何失败都阻塞合并。Performance benchmark 只记录结果，不设 pass/fail 阈值。

---

## 9. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| SQLLogicTest / sqlsmith 级别的 fuzzing | 不做 | 只测 Q1 覆盖的 SQL 子集 |
| 多 SF 测试（0.1, 1, 10）| 只用 SF=0.01 | 大数据集性能后续再测 |
| 并发/多线程测试 | 不做 | MVP 单线程 |
| 内存泄漏检测（ASan/LSan）| 建议开启但不强制 | |
| 代码覆盖率 | 不收集 | |
| TPC-H Q2-Q22 | 不测 | MVP 只支持 Q1 |
