# UmbraDB

一个受 Umbra 启发的 C++20 查询编译数据库原型，以 TPC-H Q1 为起点，探索低延迟 JIT 编译与自适应执行。

查询从 SQL 转换为物理执行计划，再通过 Tidy Tuples 风格的代码生成框架生成内部 IR。相同的 IR 可以解释执行、由 Flying Start 快速编译为 x86-64 机器码，或交给 LLVM 优化编译。自适应模式先用 Flying Start 执行，同时在后台编译 LLVM 代码，并在数据块边界切换。

## 当前能力

- **查询执行**：单表 `SELECT`、过滤、投影、`GROUP BY`、`ORDER BY`，以及 `SUM` / `AVG` / `COUNT` 聚合。
- **存储与类型**：内存列式存储，支持 TPC-H `lineitem.tbl` 加载及 Q1 所需的定点小数、日期运算。
- **执行后端**：Interpreter、Flying Start、可选 LLVM，以及自适应执行。
- **可观测性**：查看 AST、逻辑计划、物理计划、Pipeline、IR、生成代码和执行耗时。
- **交互方式**：基于 GNU Readline 的 SQL Shell，也支持从标准输入读取 SQL 并输出 TSV。

当前 Shell 使用固定的 `lineitem` 表结构，项目聚焦单表分析查询。暂不支持 JOIN、子查询、窗口函数、DML、事务或持久化存储。查询数据处理为单线程；自适应模式的 LLVM 编译在后台线程运行。

## 构建

目标平台为 **x86-64 Linux**。基础依赖：

- 支持 C++20 的 C/C++ 工具链。
- CMake 3.20+、Git、GNU Make。
- GNU Readline 开发库。

CMake 配置阶段会从 GitHub 获取 Catch2、asmjit 和 libpg_query，首次构建需要网络访问。

### 基础构建

先关闭 LLVM 后端，可运行 Interpreter 和 Flying Start：

```bash
git clone https://github.com/letian-jiang/umbra.git
cd umbra

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_BACKEND=OFF
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

可执行文件位于 `build/src/umbra`。

### 启用 LLVM

项目默认开启 `LLVM_BACKEND`。当前构建配置查找 LLVM 14，另需 `clang++` 和 `xxd`，用于生成并嵌入 host function 的 LLVM bitcode。请让 PATH 中的 `clang++` 与所选 LLVM 库版本匹配。

安装对应开发依赖后，使用独立构建目录：

```bash
cmake -S . -B build-llvm -DCMAKE_BUILD_TYPE=Release -DLLVM_BACKEND=ON
cmake --build build-llvm --parallel
ctest --test-dir build-llvm --output-on-failure
```

若 CMake 无法定位 LLVM，可通过 `-DLLVM_DIR=/path/to/llvm/lib/cmake/llvm` 指定 `LLVMConfig.cmake` 所在目录。

## 运行 TPC-H Q1

准备由 TPC-H `dbgen` 生成的 `lineitem.tbl`，放入 `data/tpch/`。数据文件需保留标准的 16 列、`|` 分隔格式；数据与生成工具需自行准备。

```text
data/
└── tpch/
    └── lineitem.tbl
```

启动 Shell：

```bash
./build/src/umbra --data data/tpch
```

默认数据目录为 `data/tpch`，也可用 `--data` 指向其他包含 `lineitem.tbl` 的目录。启动时会显示已加载行数；文件不存在时仍会进入 Shell，运行数据查询前应确认数据已正确加载。

在 Shell 中执行以下 SQL，以分号结束：

```sql
SELECT l_returnflag, l_linestatus,
       SUM(l_quantity) AS sum_qty,
       SUM(l_extendedprice) AS sum_base_price,
       SUM(l_extendedprice * (1 - l_discount)) AS sum_disc_price,
       SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,
       AVG(l_quantity) AS avg_qty,
       AVG(l_extendedprice) AS avg_price,
       AVG(l_discount) AS avg_disc,
       COUNT(*) AS count_order
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

也可以通过管道执行查询：

```bash
echo 'SELECT COUNT(*) FROM lineitem;' | ./build/src/umbra --data data/tpch
```

### 切换执行后端

| Shell 命令 | 执行方式 |
| --- | --- |
| `.backend interpreter` | 直接解释执行 IR，用于调试和结果交叉验证 |
| `.backend flying-start` | 使用 asmjit 快速生成 x86-64 机器码 |
| `.backend llvm` | 使用 LLVM O3 + ORC JIT，需启用 LLVM 构建 |
| `.backend adaptive` | Flying Start 立即执行，LLVM 后台编译完成后在 morsel 边界切换 |

Shell 默认使用 `adaptive`。未编译 LLVM 后端时，该模式使用 Flying Start 按 morsel 执行。morsel 是一批连续的数据行，默认大小为 10,000 行，可用 `.morsel 10000` 设置。

常用命令：

```text
.help                 查看命令
.tables               列出表
.schema lineitem      查看表结构
.backend              查看当前后端
.timer on             开启计时输出
.quit                 退出
```

### 查看计划与执行耗时

```sql
EXPLAIN PHYSICAL SELECT COUNT(*) FROM lineitem;
EXPLAIN PIPELINE SELECT COUNT(*) FROM lineitem;
EXPLAIN IR SELECT COUNT(*) FROM lineitem;
EXPLAIN PROFILE SELECT COUNT(*) FROM lineitem;
```

还支持 `AST`、`LOGICAL`、`ASM` 和 `ALL`。`PROFILE` 会实际执行查询并报告分阶段耗时；生成代码的查看方式见 [EXPLAIN 文档](doc/explain.md)。

## 执行架构

```text
SQL
 └─ Parser（libpg_query）→ AST
     └─ Binder → 逻辑计划
         └─ Optimizer → 物理计划 → Pipelines
             └─ Tidy Tuples 风格代码生成 → Umbra IR
                 ├─ Interpreter
                 ├─ Flying Start → x86-64
                 ├─ LLVM O3 + ORC JIT
                 └─ Adaptive：Flying Start → LLVM
```

Q1 的聚合执行拆为两个 Pipeline：第一个扫描、过滤并构建哈希聚合状态，第二个读取聚合结果、排序并输出。自适应执行通过共享的调用约定与运行时状态，在 morsel 边界替换执行代码。

## 性能记录

仓库记录了 2026-04-01 的 Q1 测试结果。以下为单线程数据处理、内存列式数据、同进程 5 次预热后 20 次运行的端到端时间，包含计划、代码生成、编译与执行，单位为 ms（均值 ± 标准差）。

| Scale factor | 行数 | Flying Start | LLVM | Adaptive |
| ---: | ---: | ---: | ---: | ---: |
| 0.01 | 60,175 | 1.83 ± 0.22 | 19.09 ± 2.31 | 1.89 ± 0.18 |
| 1 | 6,001,215 | 118.8 ± 1.0 | 41.1 ± 1.4 | 32.7 ± 0.6 |
| 10 | 59,986,052 | 1189.2 ± 6.0 | 227.0 ± 3.1 | 218.6 ± 1.9 |

这些是特定环境下的历史测量，仅覆盖 Q1。完整环境、执行时间拆分和对比数据见 [性能记录](doc/tpch-q1-results.md)，复测方法见 [Profiling 指南](doc/tpch-q1-profiling-guide.md)。

## 测试与开发

测试覆盖基础组件、IR、执行后端及 SQL 端到端查询。日常运行：

```bash
ctest --test-dir build --output-on-failure
```

DuckDB 差分测试为可选项：CMake 检测到 DuckDB 头文件与 `libduckdb.so` 后会启用，可通过 `DUCKDB_ROOT` 和 `DUCKDB_LIB_DIR` 指定路径。

提交前运行完整验证脚本（需要 LLVM 构建依赖、`clang-format` 和 `lcov`）：

```bash
./scripts/verify.sh
```

脚本执行格式化、Release 构建与测试、ASan/UBSan 测试和覆盖率检查，行覆盖率阈值为 90%。当前脚本中的 `clang-tidy` 步骤已注释。开发约定见 [CLAUDE.md](CLAUDE.md)，测试设计见 [测试文档](doc/testing.md)。

## 源码与文档

| 模块 | 源码 | 文档 |
| --- | --- | --- |
| 存储与类型 | `src/storage/`、`src/catalog/`、`src/common/` | [Storage](doc/storage.md) |
| SQL 前端 | `src/parser/`、`src/binder/` | [Parser](doc/parser.md)、[Binder](doc/binder.md) |
| 优化与计划 | `src/optimizer/` | [Optimizer](doc/optimizer.md) |
| 代码生成 | `src/codegen/` | [Tidy Tuples](doc/tidy-tuples.md) |
| 内部 IR | `src/ir/` | [IR](doc/ir.md) |
| 编译与执行 | `src/backend/`、`src/execution/` | [后端总览](doc/backend/README.md)、[自适应执行](doc/backend/adaptive-execution.md) |
| 交互与诊断 | `src/shell/`、`src/explain/` | [Shell](doc/shell.md)、[EXPLAIN](doc/explain.md) |

部分设计文档保留了早期方案与规划，使用命令和构建选项请结合当前源码确认。
