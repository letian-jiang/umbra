# TPC-H Q1 Performance Profiling Guide

如何测试和对比不同 backend 在 TPC-H Q1 上的性能。

---

## 1. 测试数据准备

### 1.1 TPC-H dbgen 数据

项目使用 `data/` 目录下的 pipe-delimited lineitem 文件：

| 文件 | Scale Factor | 行数 | 大小 |
|------|-------------|------|------|
| `data/lineitem.tbl` | SF=0.01 | ~60K | ~7MB |
| `data/lineitem_sf1.tbl` | SF=1 | ~6M | ~725MB |

UmbraDB shell 默认加载 `lineitem.tbl`。SF=1 需要用符号链接：

```bash
# 加载 SF=0.01
./build/src/umbra --data data

# 加载 SF=1
mkdir -p /tmp/umbra_sf1
ln -sf $(pwd)/data/lineitem_sf1.tbl /tmp/umbra_sf1/lineitem.tbl
./build/src/umbra --data /tmp/umbra_sf1
```

### 1.2 DuckDB 数据加载

DuckDB 直接从 .tbl 文件读取，不需要预加载：

```sql
CREATE TABLE lineitem AS SELECT * FROM read_csv('data/lineitem.tbl',
  delim='|', header=false,
  columns={
    'l_orderkey':'INTEGER','l_partkey':'INTEGER','l_suppkey':'INTEGER',
    'l_linenumber':'INTEGER','l_quantity':'DECIMAL(15,2)',
    'l_extendedprice':'DECIMAL(15,2)','l_discount':'DECIMAL(15,2)',
    'l_tax':'DECIMAL(15,2)','l_returnflag':'VARCHAR','l_linestatus':'VARCHAR',
    'l_shipdate':'DATE','l_commitdate':'DATE','l_receiptdate':'DATE',
    'l_shipinstruct':'VARCHAR','l_shipmode':'VARCHAR','l_comment':'VARCHAR'
  });
```

---

## 2. Profiling 方法

### 2.1 EXPLAIN PROFILE（推荐）

输出 per-pipeline 的 codegen / compile / execute 分阶段计时：

```sql
.backend flying-start
EXPLAIN PROFILE SELECT l_returnflag, l_linestatus,
       SUM(l_quantity), SUM(l_extendedprice),
       SUM(l_extendedprice * (1 - l_discount)),
       SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)),
       AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount),
       COUNT(*)
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

输出格式（非 adaptive）：

```
Profile (backend: flying-start):
                  plan    codegen  compile  execute  total
Pipeline 0:       -       0.02ms   0.18ms   115.02ms   115.23ms
Pipeline 1:       -       0.02ms   0.08ms   0.00ms   0.10ms
Total:           0.25ms   0.04ms   0.26ms   115.02ms   115.66ms
```

Adaptive 额外显示 morsel 分布和 LLVM 异步编译时间：

```
Profile (backend: adaptive):
                  plan    codegen  compile(FS) compile(LLVM)  execute  total
Pipeline 0:       -       0.02ms   0.27ms   13.56ms      33.16ms   33.44ms  (601 morsels)
Total:           0.25ms   0.04ms   0.42ms   33.17ms   39.91ms

  Pipeline 0: 69/601 morsels via Flying Start, 532/601 via LLVM. Switch at morsel 69.
```

**注意**：EXPLAIN PROFILE 使用默认 morsel size (10000)，不受 `.morsel` 命令影响。

### 2.2 .timer 计时

测量端到端总时间（含 parse/bind/optimize/codegen/execute），支持 `.morsel` 配置：

```
.backend adaptive
.morsel 10000
.timer on
SELECT ...;
```

输出：`4 rows (38.60 ms)`

适合 morsel size 调优实验。

### 2.3 DuckDB 对比

通过 stdin 管道执行（`.timer on` 不支持 `-c` 模式）：

```bash
# 单线程
echo "SET threads TO 1;
CREATE TABLE lineitem AS SELECT * FROM read_csv('data/lineitem.tbl', ...);
.timer on
SELECT ... FROM lineitem ...;" | duckdb

# 多线程（默认）
echo "CREATE TABLE lineitem AS SELECT * FROM read_csv('data/lineitem.tbl', ...);
.timer on
SELECT ... FROM lineitem ...;" | duckdb
```

输出：`Run Time (s): real 0.002 user 0.002356 sys 0.000000`

---

## 3. 测试矩阵

### 3.1 Backend 对比

| Backend | Shell 命令 | 特征 |
|---------|-----------|------|
| Interpreter | `.backend interpreter` | 零编译开销，纯解释执行 |
| Flying Start | `.backend flying-start` | asmjit 单趟编译，即时执行 |
| LLVM | `.backend llvm` | O3 + host 函数内联，编译慢执行快 |
| Adaptive | `.backend adaptive` | FS 立即执行 + LLVM 异步编译，morsel 边界切换 |

### 3.2 Scale Factor 对比

| SF | 行数 | 数据量 | 关注点 |
|----|------|--------|--------|
| 0.01 | 60K | ~3.4MB (fits L2) | 编译延迟占比，小数据最优 backend |
| 1 | 6M | ~336MB (far > L3) | 执行吞吐，大数据最优 backend |

### 3.3 核心指标

| 指标 | 来源 | 意义 |
|------|------|------|
| Plan time | EXPLAIN PROFILE "plan" | parse + bind + optimize |
| Codegen time | EXPLAIN PROFILE "codegen" | Tidy Tuples IR 生成 |
| Compile time | EXPLAIN PROFILE "compile" | IR → 机器码（FS 或 LLVM）|
| Execute time | EXPLAIN PROFILE "execute" | pipeline 函数执行 |
| Total time | EXPLAIN PROFILE "total" | 端到端 |
| First-row latency | compile time | 从查询提交到首行输出 |
| ns/row | execute / row_count | 单行处理速率 |
| Compile % | compile / total | 编译占端到端比重 |

Adaptive 额外指标：

| 指标 | 来源 | 意义 |
|------|------|------|
| FS morsels | EXPLAIN PROFILE morsel 行 | FS 处理的 morsel 数 |
| LLVM morsels | EXPLAIN PROFILE morsel 行 | LLVM 处理的 morsel 数 |
| Switch morsel | EXPLAIN PROFILE morsel 行 | FS→LLVM 切换点 |
| LLVM async compile | EXPLAIN PROFILE compile(LLVM) | 后台编译耗时 |

---

## 4. 批量测试脚本

### 4.1 全 backend EXPLAIN PROFILE

```bash
Q1="SELECT l_returnflag, l_linestatus, SUM(l_quantity), SUM(l_extendedprice), \
SUM(l_extendedprice * (1 - l_discount)), \
SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), \
AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), COUNT(*) \
FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY \
GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus;"

DATA_DIR=data           # SF=0.01
# DATA_DIR=/tmp/umbra_sf1  # SF=1

RUNS=7

for backend in interpreter flying-start llvm adaptive; do
  echo "=== $backend ==="
  for i in $(seq 1 $RUNS); do
    echo -e ".backend $backend\nEXPLAIN PROFILE $Q1" \
      | ./build/src/umbra --data $DATA_DIR 2>&1 \
      | grep -E "^Pipeline 0:|^Total|morsels via"
  done
done
```

### 4.2 Morsel Size 调优

```bash
for ms in 500 1000 2000 5000 10000 20000 50000 100000; do
  times=""
  for i in $(seq 1 $RUNS); do
    t=$(echo -e ".backend adaptive\n.morsel $ms\n.timer on\n$Q1" \
      | ./build/src/umbra --data $DATA_DIR 2>&1 \
      | grep "rows" | grep -oP '[\d.]+(?= ms)')
    times="$times $t"
  done
  echo "morsel=$ms: $times"
done
```

### 4.3 DuckDB 对比

```bash
DUCKDB_LOAD="CREATE TABLE lineitem AS SELECT * FROM read_csv('$DATA_DIR/lineitem.tbl',
  delim='|', header=false,
  columns={'l_orderkey':'INTEGER','l_partkey':'INTEGER','l_suppkey':'INTEGER',
    'l_linenumber':'INTEGER','l_quantity':'DECIMAL(15,2)',
    'l_extendedprice':'DECIMAL(15,2)','l_discount':'DECIMAL(15,2)',
    'l_tax':'DECIMAL(15,2)','l_returnflag':'VARCHAR','l_linestatus':'VARCHAR',
    'l_shipdate':'DATE','l_commitdate':'DATE','l_receiptdate':'DATE',
    'l_shipinstruct':'VARCHAR','l_shipmode':'VARCHAR','l_comment':'VARCHAR'});"

DUCKDB_Q1="SELECT l_returnflag, l_linestatus, SUM(l_quantity), SUM(l_extendedprice), \
SUM(l_extendedprice * (1 - l_discount)), \
SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), \
AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), COUNT(*) \
FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL 90 DAY \
GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus;"

# 单线程
for i in $(seq 1 $RUNS); do
  echo "SET threads TO 1; $DUCKDB_LOAD
.timer on
$DUCKDB_Q1" | duckdb 2>&1 | grep "Run Time"
done

# 多线程
for i in $(seq 1 $RUNS); do
  echo "$DUCKDB_LOAD
.timer on
$DUCKDB_Q1" | duckdb 2>&1 | grep "Run Time"
done
```

---

## 5. 分析要点

### 5.1 Backend 选择

- **SF=0.01**：Flying Start 最优（编译 < 0.3ms，LLVM 编译 ~17ms 完全浪费）
- **SF=1**：Adaptive ≈ LLVM（LLVM 编译被 FS 执行掩盖），均远快于 FS

### 5.2 Adaptive 分析

关注 LLVM 异步编译时间与 FS morsel 执行时间的关系：

- `switch_morsel ≈ llvm_compile_ms / fs_per_morsel_ms`
- 切换越早，更多行由快速 LLVM 处理
- Morsel size 在 1K-50K 范围内对总时间影响 <5%

### 5.3 DuckDB 对比

DuckDB 使用向量化解释（非 JIT），提供 baseline 参考：

- 单线程对比 Flying Start（同为非 O3 优化执行）
- 多线程展示并行化收益（我们 MVP 为单线程）

### 5.4 统计方法

- 每组运行 7 次，取中位数（排除 warmup 异常和 GC/调度干扰）
- 报告关键百分位：median, min, max
- 对比时使用 ns/row 归一化不同数据量
