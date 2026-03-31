# TPC-H Q1 Performance Profiling Guide

如何测试和对比不同 backend 以及DuckDB 在不同 SF 的 TPC-H 数据上的 Q1 性能。

---

## 1. 测试数据准备

### 1.1 用 dbgen 生成 TPC-H 数据

使用 TPC-H 官方 dbgen（位于 `/tmp/tpch-dbgen/`）生成任意 SF 的 lineitem.tbl，统一放在 `/tmp/umbra_sf<N>/` 下：

```bash
# 生成指定 SF 的 lineitem.tbl
generate_lineitem() {
  local sf=$1
  local dir="/tmp/umbra_sf${sf}"
  mkdir -p "$dir"
  cd /tmp/tpch-dbgen && ./dbgen -s "$sf" -T L -f
  mv /tmp/tpch-dbgen/lineitem.tbl "$dir/"
  echo "Generated $dir/lineitem.tbl (SF=$sf)"
}

# 常用 SF
generate_lineitem 0.01   # ~60K 行, ~7MB
generate_lineitem 0.1    # ~600K 行, ~72MB
generate_lineitem 1      # ~6M 行, ~725MB
generate_lineitem 10     # ~60M 行, ~7.2GB
```

> **注意**：dbgen 必须在 `/tmp/tpch-dbgen/` 目录下执行（依赖同目录的 `dists.dss` 文件），`-T L` 只生成 lineitem 表，`-f` 覆盖已有文件。

各 SF 数据规模参考：

| SF | 行数 | lineitem.tbl 大小 | 特征 |
|----|------|-------------------|------|
| 0.01 | ~60K | ~7MB | fits L2, 编译延迟主导 |
| 0.1 | ~600K | ~72MB | fits L3 |
| 1 | ~6M | ~725MB | far > L3, 执行吞吐主导 |
| 10 | ~60M | ~7.2GB | 大规模压测 |

### 1.2 加载到 UmbraDB

```bash
# 指定 SF 目录即可
SF=0.01
./build/src/umbra --data /tmp/umbra_sf${SF}
```

### 1.3 DuckDB 数据加载

DuckDB 从生成好的 .tbl 文件加载到内存表，之后查询纯内存执行：

```sql
CREATE TABLE lineitem AS SELECT * FROM read_csv('/tmp/umbra_sf1/lineitem.tbl',
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

无参启动 `duckdb`（即 `:memory:` 模式），数据加载在计时前完成，查询纯内存执行。

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
| Flying Start | `.backend flying-start` | asmjit 单趟编译，即时执行 |
| LLVM | `.backend llvm` | O3 + host 函数内联，编译慢执行快 |
| Adaptive | `.backend adaptive` | FS 立即执行 + LLVM 异步编译，morsel 边界切换 |

### 3.2 Scale Factor 对比

| SF | 行数 | 数据量 | 关注点 |
|----|------|--------|--------|
| 0.01 | 60K | ~7MB (fits L2) | 编译延迟占比，小数据最优 backend |
| 0.1 | 600K | ~72MB (fits L3) | 编译/执行平衡点 |
| 1 | 6M | ~725MB (far > L3) | 执行吞吐，大数据最优 backend |
| 10 | 60M | ~7.2GB | 大规模压测，纯吞吐对比 |

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

所有 warmup + hot run 在**同一个进程**内完成（数据加载一次，查询多次）。相邻 run 之间插入 `.sleep 100` 等待 100ms，确保 adaptive 后台 LLVM 编译完成，避免 cache 竞争干扰后续测量：

```bash
Q1="SELECT l_returnflag, l_linestatus, SUM(l_quantity), SUM(l_extendedprice), \
SUM(l_extendedprice * (1 - l_discount)), \
SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)), \
AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount), COUNT(*) \
FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY \
GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus;"

SF=0.01                          # 改为 0.1 / 1 / 10 测试其他 SF
DATA_DIR=/tmp/umbra_sf${SF}

WARMUP=5
RUNS=20

for backend in flying-start llvm adaptive; do
  echo "=== $backend ==="
  # 构建单进程输入：warmup 查询（无 EXPLAIN PROFILE）+ hot run 查询
  # 每次查询后 sleep 100ms，防止 adaptive 后台 LLVM 编译累积导致 cache 干扰
  {
    echo ".backend $backend"
    for i in $(seq 1 $WARMUP); do echo "$Q1"; echo ".sleep 100"; done
    for i in $(seq 1 $RUNS); do echo "EXPLAIN PROFILE $Q1"; echo ".sleep 100"; done
  } | ./build/src/umbra --data $DATA_DIR 2>&1 \
    | grep -E "Pipeline [01]:|Total:|morsels via"
done
```

### 4.2 Morsel Size 调优

```bash
for ms in 500 1000 2000 5000 10000 20000 50000 100000; do
  {
    echo ".backend adaptive"
    echo ".morsel $ms"
    # Warmup（无 timer）
    for i in $(seq 1 $WARMUP); do echo "$Q1"; echo ".sleep 100"; done
    # Hot runs（开启 timer）
    echo ".timer on"
    for i in $(seq 1 $RUNS); do echo "$Q1"; echo ".sleep 100"; done
  } | ./build/src/umbra --data $DATA_DIR 2>&1 \
    | grep -oP '[\d.]+(?= ms)' \
    | paste -sd' ' \
    | xargs -I{} echo "morsel=$ms: {}"
done
```

### 4.3 DuckDB 对比

DuckDB 同样在**同一个进程**内完成所有 warmup + hot run。从 .tbl 文件加载到内存表后，查询纯内存执行：

```bash
DUCKDB_LOAD="CREATE TABLE lineitem AS SELECT * FROM read_csv('${DATA_DIR}/lineitem.tbl',
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

for mode in "单线程:SET threads TO 1;" "多线程:"; do
  label="${mode%%:*}"
  setup="${mode#*:}"
  echo "=== DuckDB $label ==="
  {
    echo "$setup"
    echo "$DUCKDB_LOAD"
    # Warmup（无 timer）
    for i in $(seq 1 $WARMUP); do echo "$DUCKDB_Q1"; done
    # Hot runs（开启 timer）
    echo ".timer on"
    for i in $(seq 1 $RUNS); do echo "$DUCKDB_Q1"; done
  } | duckdb 2>&1 | grep "Run Time"
done
```

---

## 5. 统计方法

- 所有 warmup + hot run 在**同一个进程**内完成：数据只加载一次，避免重复磁盘 I/O 和进程启动开销
- 相邻 run 之间插入 100ms 间隔（`.sleep 100`），确保 adaptive 后台 LLVM 编译任务完成，避免累积的后台编译线程与前台执行竞争 CPU cache
- 先运行 5 次 warmup 查询（丢弃结果），预热进程内 icache / dcache / branch predictor / JIT 缓存
- 再运行 20 次 hot run，报告 mean ± std_dev（样本标准差）
- 对比时使用 ns/row 归一化不同数据量
