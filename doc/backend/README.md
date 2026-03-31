# Backend Overview

## 1. Architecture

Umbra IR Program 是所有后端的共同输入。每个后端将 IR 翻译为可执行代码（或直接解释执行），产出一个函数指针（或直接执行结果）。

```
Umbra IR Program
     │
     ├── Interpreter ─────── 逐条解释执行（调试用）
     │
     ├── Flying Start ────── asmjit 直接生成 x86-64（低延迟）
     │
     ├── LLVM Backend ────── LLVM O3 + ORC JIT（高性能）
     │
     └── Adaptive ────────── Flying Start + async LLVM（生产默认）
              │
              ├── 立即用 Flying Start 执行
              ├── 后台 LLVM 编译
              └── morsel 边界切换
```

## 2. Backend 枚举

```cpp
enum class Backend {
    INTERPRETER,    // 解释执行，零编译开销
    FLYING_START,   // 单次快速编译，直接执行
    LLVM,           // LLVM O3 编译，最优执行性能
    ADAPTIVE,       // Flying Start + async LLVM，morsel 边界切换
};
```

## 3. 统一接口

所有后端共享：

- **输入**: `ir::Program` + `ir::FunctionId`
- **函数签名**: `(PTR, INT64, INT64) → VOID`（RuntimeContext + startRow + endRow）
- **调用约定**: System V AMD64 ABI，通过 `execute()` trampoline 调用
- **ABI 兼容**: 任意后端编译的函数指针可以互相替换（adaptive execution 的基础）

## 4. 后端对比

| | Interpreter | Flying Start | LLVM | Adaptive |
|---|---|---|---|---|
| 编译时间 | 0 | ~0.15ms | ~5-20ms | FS 即时 + LLVM 后台 |
| 执行性能 | 最慢（~10x） | 快（baseline） | 最快（~1.2x FS） | FS → LLVM 切换 |
| 用途 | 调试、正确性验证 | 短查询、开发 | Benchmark | **生产默认** |
| 论文对应 | 无（MVP 特有） | Tidy Tuples §2 | §2.6, §5.4 | §4.1, §5.4 |

## 5. 文档索引

| 文档 | 内容 |
|------|------|
| [interpreter.md](interpreter.md) | 解释执行后端：value slots、逐指令求值、PHI 解析 |
| [flying-start.md](flying-start.md) | Flying Start JIT：live span 分析、寄存器分配、on-the-fly 优化 |
| [llvm-backend.md](llvm-backend.md) | LLVM 后端：IR lowering、O3 优化、ORC JIT |
| [inline-host-functions.md](inline-host-functions.md) | Host function 内联：LLVM 后端将 C++ host 函数内联到生成代码中 |
| [adaptive-execution.md](adaptive-execution.md) | 自适应执行：morsel-driven dispatch、AsyncCompiler、FS→LLVM 切换 |

## 6. 源码对应

| 后端 | 源码路径 |
|------|---------|
| Interpreter | `src/backend/interpreter/` |
| Flying Start | `src/backend/flying_start/` |
| LLVM | `src/backend/llvm/` |
| Adaptive Execution | `src/execution/` |
