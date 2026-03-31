# Adaptive Execution Design Document

## 1. Overview

Adaptive Execution 是论文 §4.1 和 §5.4 描述的核心策略：**查询开始时立即用 Flying Start 执行，同时在后台用 LLVM 编译优化代码；当 LLVM 编译完成后，在 morsel 边界切换到 LLVM 代码继续执行**。这样短查询享受 Flying Start 的低延迟，长查询在 LLVM 编译完成后享受更快的执行速度。

> **论文 Figure 14**: Umbra 只需在 Flying Start 和 LLVM 两个后端之间切换（对应图中 "Umbra modes"）。Flying Start 编译速度与 HyPer 字节码解释器相当，执行速度与 HyPer unoptimized LLVM 相当。因此 Umbra 用一个后端（Flying Start）覆盖了 HyPer 两个后端（bytecode + LLVM O0）的设计空间。

```
Query Plan
    │
    ├── Tidy Tuples Codegen → Umbra IR Program
    │
    ├── Pipeline 0 (build): morsel-driven execution
    │    │
    │    ├── [instant] Flying Start compile → start executing morsels
    │    │
    │    ├── [background] LLVM compile (async thread)
    │    │
    │    └── [morsel boundary] if LLVM ready → switch to LLVM code
    │                          else → continue with Flying Start
    │
    └── Pipeline 1 (scan): morsel-driven over hash table slots, same adaptive strategy
```

### 设计原则

1. **Morsel-driven execution**: 将 table scan 按 morsel（行块）分割，每个 morsel 是独立的执行单元，提供切换点
2. **Asynchronous compilation**: LLVM 编译在独立线程上运行，不阻塞 Flying Start 的执行
3. **Zero-overhead switching**: 切换只发生在 morsel 边界，无需中断正在执行的代码或传递指令级状态
4. **Always-correct fallback**: 如果 LLVM 编译在 pipeline 完成前未结束，整个 pipeline 用 Flying Start 完成——不会更慢
5. **共享 RuntimeContext**: Flying Start 和 LLVM 生成的代码共享相同的 ABI（`i64(i64, ...)`），共享 RuntimeContext、hash table 等运行时状态

---

## 2. Morsel-Driven Execution

### 2.1 当前问题

当前的 table scan 是单次全量循环：`for i = 0 to rowCount`。整个 pipeline function 一次调用处理所有行。这意味着：
- 无法在中途切换后端
- 无法在中途收集运行时统计
- 为未来的并行执行（morsel-driven parallelism）也留不出接口

### 2.2 Morsel 概念

将 table scan 的行范围 `[0, rowCount)` 分割为固定大小的 morsel：

```
Morsel size = 10,000 rows (可调)

Table: 60,175 rows (SF=0.01)
  Morsel 0: [0, 10000)
  Morsel 1: [10000, 20000)
  ...
  Morsel 5: [50000, 60000)
  Morsel 6: [60000, 60175)   ← 尾部 morsel，不足整块
```

### 2.3 Pipeline Function 签名变更

当前签名（处理全部行）：

```cpp
// void pipeline_build(RuntimeContext* ctx)
// 内部循环: for i = 0 to ctx->rowCount
```

Morsel 化签名（处理指定行范围）：

```cpp
// void pipeline_build(RuntimeContext* ctx, uint64_t startRow, uint64_t endRow)
// 内部循环: for i = startRow to endRow
```

参数传递：使用现有的 `FUNARG` 机制，函数签名变为 `(PTR, INT64, INT64) → VOID`。`startRow` 和 `endRow` 作为第 2、3 个参数。

**所有后端、所有 pipeline 统一使用 morsel 签名**：`(PTR, INT64, INT64) → VOID`。非 adaptive 模式由 executor 传入 `(ctx, 0, totalItems)` 一次性执行全部。这样每个 pipeline 的 codegen 都只有一条路径，无需区分 build/scan pipeline。

- Build pipeline：`startRow`/`endRow` 对应 table row 范围
- Scan pipeline：`startRow`/`endRow` 对应 hash table slot index 范围

### 2.4 Codegen 变更

所有 pipeline 的循环上下界都从函数参数读取 `startRow`/`endRow`：

**Build pipeline** — `TableScanTranslator::produce()` 修改：

```
// 当前:
auto rowCount = load(rtCtx + 8);    // from RuntimeContext
auto zero = const(0);
loop(zero, rowCount) { ... }

// 变更为:
auto startRow = funarg(1);          // 第 2 个参数
auto endRow = funarg(2);            // 第 3 个参数
loop(startRow, endRow) { ... }
```

**Scan pipeline** — `HashTableCodegen::scan()` 修改：

```
// 当前:
auto capacity = call(getCapacityFn, htPtr);  // from host function
auto zero = const(0);
loop(zero, capacity) { ... }

// 变更为:
auto startSlot = funarg(1);         // 第 2 个参数
auto endSlot = funarg(2);           // 第 3 个参数
loop(startSlot, endSlot) { ... }
```

Executor 传入 `(ctx, 0, htCapacity)` 或 morsel 范围。`htCapacity` 通过 `HashTable::capacity()` 在 host 端获取。

### 2.5 Morsel 大小选择

| 因素 | 考虑 |
|------|------|
| 太小（< 1K） | Morsel dispatch 开销显著（函数调用、cache cold start） |
| 太大（> 100K） | 切换粒度太粗，LLVM 可能错过切换窗口 |
| 甜蜜点：10K | 每 morsel ~0.05ms（Flying Start Q1），dispatch 开销 < 1%；SF=0.01 产生 ~6 个 morsel |

默认 morsel size = 10,000 行。可通过 shell 命令 `.morsel <size>` 或 `ShellConfig` 配置。

> **未来扩展**: morsel 是 morsel-driven parallelism 的基础单元。当实现多线程时，多个 worker 从共同的 morsel queue 取 morsel 执行。Adaptive execution 的 morsel 基础设施可以直接复用。

---

## 3. Asynchronous LLVM Compilation

### 3.1 编译时序

```
Timeline (Q1, SF=0.01, pool warm):

t=0.00ms  Codegen starts (Tidy Tuples → IR)
t=0.02ms  Codegen done
t=0.02ms  LLVM compile submitted to pool (near-zero: mutex lock + notify)
t=0.02ms  Flying Start compile (fast, ~0.19ms)
t=0.21ms  Flying Start ready → begin executing morsels
t=0.21ms  Morsel 0 [0, 10K) via Flying Start
t=0.37ms  Morsel 1 [10K, 20K) via Flying Start
  ...
t=1.35ms  Morsel 6 done — all 60K rows processed via Flying Start
          (LLVM still compiling in pool, ~13ms total)
t=1.35ms  Pipeline 0 complete. ~AsyncCompiler() releases shared_ptr, non-blocking.
          Pool worker continues compilation, cleans up when done.
```

```
Timeline (hypothetical Q1, SF=1, 6M rows):

t=0.00ms  Codegen → IR → Flying Start compile
t=0.20ms  Flying Start ready + LLVM compile starts
t=0.20ms  Morsel 0-9 via Flying Start (~0.5ms each)
t=5.20ms  ~10 morsels done via Flying Start (~100K rows)
t=5.20ms  LLVM compilation completes → mark as ready
t=5.20ms  Morsel 10+ via LLVM (faster per-morsel)
  ...
t=48ms    All 600 morsels done (mix of FS + LLVM)
```

### 3.2 CompileThreadPool

常驻线程池，大小 `hardware_concurrency()`（通常 = `nproc`）。Worker 线程在 `condition_variable` 上等待任务，提交只需 notify（无 `pthread_create` 开销）。

```cpp
class CompileThreadPool {
public:
    static CompileThreadPool& instance();  // Meyer's singleton, lazy init
    void submit(std::function<void()> task);
    ~CompileThreadPool();  // shutdown flag + notify_all + join

private:
    explicit CompileThreadPool(unsigned numThreads);
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_{false};
};
```

- Shell 启动时调用 `CompileThreadPool::instance()` 预热（`Shell` 构造函数），避免首个查询承担线程创建开销
- 冷启动开销 ~1ms（创建 nproc 个线程），热启动 ~0（只是 mutex lock + notify）
- Worker 线程空闲时仅消耗虚拟地址空间（栈未触及不占物理内存），CPU 占用为零

### 3.3 AsyncCompiler

封装 LLVM 后台编译的提交和轮询：

```cpp
class AsyncCompiler {
public:
    /// Submit LLVM compilation to the thread pool.
    void startAsync(std::shared_ptr<ir::Program> program, ir::FunctionId funcId);

    /// Check if LLVM code is ready (non-blocking, lock-free).
    [[nodiscard]] bool isReady() const;

    /// Get the compiled function pointer. Must only call after isReady() == true.
    [[nodiscard]] void* getJitFunc() const;

    /// Non-blocking — pool worker owns SharedState via shared_ptr.
    ~AsyncCompiler() = default;

private:
    /// Shared state: ref-counted, pool worker holds its own reference.
    /// When AsyncCompiler is destroyed, worker keeps state alive until done.
    struct SharedState {
        std::atomic<bool> ready{false};
        bool failed{false};
        void* jitFunc{nullptr};
        double compileMs{0.0};
        std::shared_ptr<ir::Program> program;
        std::unique_ptr<llvm_backend::LLVMCompiler> compiler;
    };

    std::shared_ptr<SharedState> state_;
    bool started_{false};
};
```

实现要点：
- `startAsync()` 提交编译任务到 `CompileThreadPool`，lambda 捕获 `shared_ptr<SharedState>`（而非 `this`），使 pool worker 可以安全地 outlive `AsyncCompiler` 对象
- `isReady()` 通过 `std::atomic<bool>` 检查，**无锁**，morsel 边界调用不引入 contention
- **析构函数非阻塞**：`~AsyncCompiler()` 只释放 `shared_ptr<SharedState>` 的引用。如果 pool worker 还在编译，它持有自己的引用，编译完成后自动清理
- **无需拷贝 `ir::Program`**：经验证，Flying Start 和 LLVM 两个后端的 `compile()` 均不修改 Program（所有访问都是只读的：`functionMeta()`、`instructionAt()` 等）。两个后端共享同一个 `const ir::Program&`，并发读无 data race
- 后续应将所有后端的 `compile()` 签名改为 `const ir::Program&` 以 enforce 不可变约束
- **生命周期**：`SharedState` 通过引用计数管理。pool worker lambda 和 `AsyncCompiler` 各持一份 `shared_ptr<SharedState>`。最后一个引用释放时，`LLVMCompiler` 和 `ir::Program` 自动析构

### 3.4 线程安全

| 共享资源 | 线程 | 安全性 |
|---------|------|--------|
| `ir::Program` | 共享 const 引用，并发只读 | `std::vector` 并发读无 data race，安全 |
| `RuntimeContext` | 仅执行线程访问 | 无共享，安全 |
| `HashTable` | 仅执行线程访问 | 无共享，安全 |
| `ready_` flag | 编译线程写、执行线程读 | `std::atomic<bool>`，安全 |
| LLVM 全局状态 | 编译线程 | LLVM context per-thread，安全 |

关键不变量：**编译线程只做编译，不访问运行时数据；执行线程在 morsel 边界原子读 ready flag，不接触编译状态**。

---

## 4. Morsel Dispatch Loop

### 4.1 执行框架

Pipeline 执行从"一次函数调用处理所有行"变为"循环调度 morsel"：

```cpp
void PipelineExecutor::executeBuildPipelineAdaptive(
    const optimizer::OptimizeResult& result, Table& table)
{
    // Phase 1: Codegen (shared IR, same as before)
    auto program = codegen(...);

    // Phase 2: Flying Start compile (instant)
    auto fsFunc = flyingStartCompile(program, funcId);

    // Phase 3: Start async LLVM compile (background, shares program by const ref)
    AsyncCompiler asyncLlvm;
    asyncLlvm.startAsync(program, funcId);  // no copy needed, concurrent read-only

    // Phase 4: Morsel dispatch loop
    RuntimeContext rtCtx = setupRuntimeContext(table);
    uint64_t totalRows = table.rowCount;
    uint64_t morselSize = config_.morselSize;  // default 10000
    uint64_t cursor = 0;

    void* activeFunc = fsFunc;  // start with Flying Start

    while (cursor < totalRows) {
        uint64_t end = std::min(cursor + morselSize, totalRows);

        // Execute one morsel
        std::vector<uint64_t> args = {rtCtxArg, cursor, end};
        FlyingStartCompiler::execute(activeFunc, args);  // same ABI for both

        cursor = end;

        // Check if LLVM is ready at morsel boundary
        if (activeFunc == fsFunc && asyncLlvm.isReady()) {
            activeFunc = asyncLlvm.getJitFunc();
            // From now on, all remaining morsels use LLVM code
        }
    }

    // ~AsyncCompiler() is non-blocking: just releases shared_ptr.
    // Pool worker holds its own reference, cleans up when compilation finishes.
}
```

### 4.2 ABI 兼容

Flying Start 和 LLVM 生成的函数共享相同的调用约定：

```
函数签名: i64(i64 rtCtx, i64 startRow, i64 endRow)
调用方式: ((uint64_t(*)(uint64_t, uint64_t, uint64_t))funcPtr)(rtCtx, start, end)
```

两者都由 `execute()` trampoline 调用，使用 System V AMD64 ABI。因此 `activeFunc` 指针可以在 morsel 边界无缝替换——调用方不感知后端差异。

### 4.3 状态连续性

Morsel 之间需要保持连续的状态：

| 状态 | 存储位置 | Pipeline | 连续性保证 |
|------|---------|----------|-----------|
| Hash table 内容 | `RuntimeContext.hashTable` 指向堆内存 | Build | 两个后端写入同一个 hash table，插入顺序不影响正确性 |
| Scan cursor | 外部 morsel dispatch loop | Both | 由 `startRow`/`endRow` 参数显式传递 |
| Result buffer | `RuntimeContext.resultBuffer` | Scan | morsel 间追加写入，`resultRowCount` 单调递增 |
| `resultRowCount` | `RuntimeContext.resultRowCount` | Scan | 每个 morsel 从当前值继续累加 |

关键洞察：
- **Build pipeline**: hash table 的插入是无序的（基于 hash key），不同后端的交错写入不影响最终结果
- **Scan pipeline**: hash table slot 遍历是独立的（每个 slot 要么 occupied 要么不是），morsel 分割不影响正确性。`resultRowCount` 在 morsel 间保持连续（通过 RuntimeContext 传递）

---

## 5. Always-Race Strategy

### 5.1 策略

ADAPTIVE 模式下，**每个 pipeline 总是同时启动 FS 和 LLVM 编译**，无需决策逻辑：

- FS 编译完成 → 立即开始执行 morsel
- LLVM 在某个 morsel 边界编译完成 → 切换到 LLVM 代码
- 所有 morsel 执行完毕 → pipeline 结束，**不等待 LLVM 编译完成**

关键约束：**LLVM 编译不能阻塞查询结束**。如果 pipeline 在 LLVM 编译完成前就处理完了所有行，直接返回结果。`AsyncCompiler` 析构函数只释放 `shared_ptr<SharedState>` 引用（非阻塞），pool worker 持有自己的引用继续编译直到完成后自动清理。

> **论文 §5.4**: "Umbra, on the other hand, only has to choose from two backends... it is safe to always begin execution with Flying Start and, if necessary, shift into high gear by using the optimizing compiler."

### 5.2 Per-Pipeline 行为

每个 pipeline 独立执行 always-race strategy，统一 morsel-driven：

**Pipeline 0 (build)**: TableScan → HashAggregate
- morsel 范围 = table row 范围 `[0, rowCount)`
- SF=0.01（60K 行，~6 morsel）：LLVM 大概率来不及，全部 FS 执行
- SF=1（6M 行，~600 morsel）：LLVM 在 ~85 morsel 后编译完成，剩余 morsel 用 LLVM

**Pipeline 1 (scan)**: HashAggregate(scan) → Projection → Sort → ResultWriter
- morsel 范围 = hash table slot 范围 `[0, htCapacity)`
- Q1 的 htCapacity 很小（~16 slots），仅 1 个 morsel，FS 瞬间完成
- LLVM 大概率来不及，直接丢弃

### 5.3 参数

| 参数 | 默认值 | 含义 |
|------|--------|------|
| `morselSize` | 10,000 | 每个 morsel 的 item 数（row 或 slot） |

---

## 6. Backend Enum 与 Shell 集成

### 6.1 Backend 枚举扩展

```cpp
enum class Backend {
    INTERPRETER,    // Reference interpreter (debug only)
    FLYING_START,   // Flying Start JIT only
    LLVM,           // LLVM only (wait for compilation)
    ADAPTIVE,       // Flying Start + async LLVM (production default)
};
```

### 6.2 Shell 命令

```
umbra> .backend adaptive
Backend set to: adaptive

umbra> .backend
Current backend: adaptive

umbra> .morsel 20000
Morsel size set to: 20000
```

### 6.3 EXPLAIN PROFILE 扩展

```
Profile (backend: adaptive, SF=0.01, actual measurement):
                  plan    codegen  compile(FS) compile(LLVM)  execute  total
Pipeline 0:       -       0.02ms   0.19ms      (bg, unused)   1.14ms   1.35ms  (7 morsels)
Pipeline 1:       -       0.01ms   0.08ms      (bg, unused)   0.00ms   0.09ms  (1 morsels)
Total:           0.23ms   0.03ms   0.27ms      -              1.14ms   1.76ms

  Pipeline 0: 7/7 morsels via Flying Start, 0/7 via LLVM. LLVM detached.
  Pipeline 1: 1/1 morsels via Flying Start, 0/1 via LLVM. LLVM detached.

Profile (backend: adaptive, SF=1, hypothetical):
                  plan    codegen  compile(FS) compile(LLVM)  execute  total
Pipeline 0:       -       0.02ms   0.19ms      13.0ms         33.2ms   33.4ms  (601 morsels)
Pipeline 1:       -       0.01ms   0.08ms      (bg, unused)   0.01ms   0.10ms  (1 morsels)
Total:           0.23ms   0.03ms   0.27ms      13.0ms         33.2ms   33.5ms

  Pipeline 0: 69/601 morsels via Flying Start, 532/601 via LLVM. Switch at morsel 69.
  Pipeline 1: 1/1 morsels via Flying Start, 0/1 via LLVM. LLVM detached.
```

### 6.4 PipelineProfile 扩展

```cpp
struct PipelineProfile {
    double codegenMs{0.0};
    double compileMs{0.0};       // Flying Start compile time
    double llvmCompileMs{0.0};   // LLVM compile time (0 if still running when pipeline ended)
    double executeMs{0.0};

    // Adaptive execution stats
    uint32_t totalMorsels{0};
    uint32_t fsMorsels{0};       // Morsels executed via Flying Start
    uint32_t llvmMorsels{0};     // Morsels executed via LLVM
    uint32_t switchMorsel{0};    // Morsel index where switch happened (0 = no switch)
};
```

---

## 7. Scan Pipeline（Pipeline 1）的 Morsel 化

### 7.1 当前结构

Pipeline 1（scan side）遍历 hash table 中的 group，不做 table scan。遍历逻辑在 `HashAggregateTranslator::produceScan()` 中生成，循环 `i = 0 to capacity` 遍历 hash table 的 slot array，检查 occupied flag。

### 7.2 Morsel 化

Pipeline 1 与 Pipeline 0 使用相同的 morsel 框架：

- **函数签名**：`(PTR, INT64, INT64) → VOID`，与 build pipeline 一致
- **Morsel 范围**：`[startSlot, endSlot)` 对应 hash table slot index
- **Codegen 变更**：`HashTableCodegen::scan()` 从 `funarg(1)`/`funarg(2)` 读取循环边界，替代 `call(getCapacityFn)`
- **Executor 传入**：`totalItems = hashTable.capacity()`，通过 host 端获取

对 Q1，htCapacity 很小（~16 slots），远小于 morselSize，实际只产生 1 个 morsel。但统一签名和调度逻辑简化了整体设计，无需对 pipeline 类型做条件分支。

---

## 8. IR Program 共享

### 8.1 无需拷贝

经验证，Flying Start 和 LLVM 两个后端的 `compile()` **均不修改** `ir::Program`。所有访问都是只读的（`functionMeta()`、`instructionAt()`、`blockAt()`、`hostFunction()` 等）。因此 FS 编译线程和 LLVM 后台编译线程可以安全地共享同一个 `const ir::Program&`，并发只读访问 `std::vector` 无 data race。

### 8.2 生命周期

Pipeline 先结束的场景（LLVM 未完成）：`AsyncCompiler` 析构时只释放 `shared_ptr<SharedState>`。Pool worker 持有自己的引用，继续编译直到完成，然后释放最后一个引用触发资源清理。**不阻塞查询结果返回**。

Pipeline 后结束的场景（LLVM 已切换）：`isReady()` 已返回 true，pool worker 已完成。`AsyncCompiler` 析构时释放最后一个引用，资源立即清理。

### 8.3 Const 正确性

后续应将所有后端的 `compile()` 签名从 `ir::Program&` 改为 `const ir::Program&`，以编译期 enforce 不可变约束。当前签名接受非 const 引用是历史遗留，实际无写操作。

---

## 9. 与现有后端的关系

### 9.1 后端矩阵

| Backend | 编译 | 执行 | 适用场景 |
|---------|------|------|---------|
| `INTERPRETER` | 无 | 解释执行，`(ctx, 0, totalItems)` | 调试、正确性验证 |
| `FLYING_START` | 单次 FS | FS 执行，`(ctx, 0, totalItems)` | 短查询、开发调试 |
| `LLVM` | 单次 LLVM | LLVM 执行，`(ctx, 0, totalItems)` | Benchmark、对比 |
| `ADAPTIVE` | FS + async LLVM | morsel 循环，FS → LLVM 切换 | **生产默认** |

所有后端、所有 pipeline 共享同一 codegen 路径和 `(PTR, INT64, INT64) → VOID` 签名。区别仅在 executor 层：非 adaptive 模式传入 `(ctx, 0, totalItems)` 单次调用；adaptive 模式循环调用多个 morsel。`totalItems` 在 build pipeline 中是 `rowCount`，在 scan pipeline 中是 `htCapacity`。

### 9.2 向后兼容

- `INTERPRETER`、`FLYING_START`、`LLVM` 行为变更：所有 pipeline 签名从 `(PTR) → VOID` 变为 `(PTR, INT64, INT64) → VOID`，executor 传入 `(ctx, 0, totalItems)` 保持语义一致
- `ADAPTIVE` 是新增模式，使用 morsel dispatch loop
- 所有 backend 共享同一份 codegen 代码，无条件分支
- 现有 test 需要更新调用方式（传 `startRow`/`endRow` 参数），但执行结果不变

### 9.3 代码复用

| 组件 | 复用情况 |
|------|---------|
| Tidy Tuples codegen | `TableScanTranslator` 和 `HashTableCodegen::scan()` 统一使用 morsel 参数 |
| IR Program & Builder | 共享 const 引用，`compile()` 签名改为 `const ir::Program&` |
| Flying Start compiler | 完全复用 |
| LLVM compiler | 完全复用（在独立线程上调用） |
| `execute()` trampoline | 完全复用（morsel 参数透传） |
| RuntimeContext | 结构不变，morsel dispatch 在外部 |

---

## 10. 实现计划

### Phase 1: Morsel-Driven 所有 Pipeline（所有后端）

1. 修改 `TableScanTranslator::produce()` 从函数参数读取 `startRow`/`endRow`（替代 RuntimeContext 中的 `rowCount`）
2. 修改 `HashTableCodegen::scan()` 从函数参数读取 `startSlot`/`endSlot`（替代 `call(getCapacityFn)`）
3. 修改所有 pipeline 签名为 `(PTR, INT64, INT64) → VOID`（所有后端统一）
4. 修改 `PipelineExecutor` 的所有后端调用：`INTERPRETER`/`FLYING_START`/`LLVM` 传入 `(ctx, 0, totalItems)` 单次调用（build: `totalItems=rowCount`，scan: `totalItems=htCapacity`）
5. 验证所有后端结果一致（differential test：morsel 签名 vs 现有 DuckDB baseline）

### Phase 2: Morsel Dispatch Loop + AsyncCompiler + CompileThreadPool

6. 在 `PipelineExecutor` 中为 `ADAPTIVE` 模式实现 morsel dispatch loop（所有 pipeline 统一，复用 Phase 1 的 morsel 签名）
7. 实现 `CompileThreadPool` 常驻线程池（`nproc` 个 worker，condition variable 调度）
8. 实现 `AsyncCompiler` 类（`shared_ptr<SharedState>` + `std::atomic`，非阻塞析构）
9. 在 morsel dispatch loop 中集成 LLVM ready 检测和切换
10. Shell 构造函数中预热线程池（`CompileThreadPool::instance()`）
11. 验证 adaptive 执行结果与单后端一致

### Phase 3: Shell 与 EXPLAIN 集成

12. 扩展 `Backend` enum 添加 `ADAPTIVE`
13. Shell `.backend adaptive` 命令
14. EXPLAIN PROFILE 显示 adaptive 统计

### Phase 4: 调优与测试

15. Morsel size 参数化
16. 端到端测试：SF=0.01（全 FS，LLVM 来不及）、SF=0.1（可能切换）、SF=1（确定切换）
17. 性能回归测试：确认 adaptive 模式下 SF=0.01 仍然 < 3.1ms（LLVM 编译不阻塞）——✅ 实测 1.76ms

---

## 11. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| Morsel-driven parallelism（多线程） | 单线程 morsel dispatch | 仅用于 adaptive 切换，不并行 |
| Per-morsel cost model 动态调整 | 固定 morsel size | 简单，Q1 足够 |
| 多 pipeline 并发编译 | 串行 pipeline，每个 pipeline 独立 adaptive | Q1 只有 2 个 pipeline |
| LLVM 部分编译（function 级别） | 整个 program 一起编译 | Q1 每个 pipeline 只有一个 function |
| 编译结果缓存（相同查询复用） | 不缓存 | MVP 无 prepared statement |
| Interpreter → FS adaptive（3 级） | 只做 FS → LLVM adaptive（2 级） | Interpreter 仅用于调试 |
