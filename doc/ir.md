# Umbra IR Design Document

## 1. Overview

Umbra IR is the custom intermediate representation that sits between the Tidy Tuples code generation framework and the compilation backends. It serves as the single program representation that all backends consume.

论文描述了两个 machine-code backend：**Flying Start**（低延迟直接 x86 生成）和 **LLVM optimizing backend**（高吞吐优化编译），见论文 Fig.4 和 §4.1。

> **[MVP deviation]** MVP 不实现 LLVM backend，只实现 Flying Start。另外新增一个 **Interpreter** 作为调试和正确性验证的参考实现——这不是论文的设计，而是 MVP 为降低开发风险而引入的。

### Design Goals

1. **Fast to write**: code generation appends instructions sequentially; no instruction reordering or insertion
2. **Fast to read**: compact, cache-friendly layout; backends scan instructions linearly
3. **Low memory allocation**: contiguous buffers, minimal pointer chasing
4. **SSA form**: generated directly by the Codegen API layer; no extra pass to convert

### Comparison to LLVM IR

| Aspect                  | LLVM IR                          | Umbra IR                          |
|-------------------------|----------------------------------|-----------------------------------|
| Design goal             | General-purpose optimization     | Fast write & read for JIT         |
| Instruction storage     | Linked list per basic block      | Flat byte array, append-only      |
| Instruction reference   | Pointer (8 bytes)                | Offset (4 bytes)                  |
| Instruction format      | Fixed-size C++ objects           | Variable-length, packed           |
| SSA construction        | Separate pass or mem2reg         | Direct, via Codegen API           |
| Optimization support    | Reorder, replace, delete freely  | Append-only; constant fold + DCE  |
| Memory allocation       | Per-instruction heap allocation  | Bulk arena, rarely allocates      |

Umbra IR trades away the flexibility to arbitrarily transform programs in exchange for optimal writing and reading speed.

---

## 2. Program Structure

A program is a hierarchy of three levels:

```
Program
 ├── Function 0
 │    ├── BasicBlock 0  (entry)
 │    │    ├── Instruction 0
 │    │    ├── Instruction 1
 │    │    └── ... (terminator: br/condbr/ret/switch)
 │    ├── BasicBlock 1
 │    │    └── ...
 │    └── ...
 ├── Function 1
 │    └── ...
 └── ...
```

### Functions

- Each function has a name (for debugging), a return type, a list of parameter types, and a pointer to its entry basic block.
- A function is identified by its index in the program's function table.
- A query pipeline maps to one function. A typical query has 2-3 functions.

### Basic Blocks

- Each basic block holds a **dynamic array of instruction offsets** pointing into the instruction buffer. The offsets determine which instructions belong to this block and their execution order.
- The instructions of a block are not required to be contiguous in the instruction buffer (though in practice they usually are, because code generation has high locality).
- Every basic block must be terminated by exactly one control-flow instruction (`br`, `condbr`, `ret`, or `switch`).
- A function only stores the **offset of its entry basic block**. All other basic blocks within the function are discoverable by following the branch targets from the entry block — there is no explicit block list per function.

### Instructions

- Each instruction produces at most one result value (or void for stores/branches/returns).
- An instruction is identified by its **offset** in the instruction byte buffer — this serves as both its "name" (like LLVM's `%1`) and its address.
- Instructions reference their operands by offset.

---

## 3. Physical Memory Layout

一个 `Program` 由三个连续的动态数组组成，设计上完全平行（论文 §3.2）：

| Buffer             | 内容                                     | 元素格式               | 引用方式        |
|--------------------|------------------------------------------|-----------------------|----------------|
| Instruction Buffer | 所有指令                                 | 变长（opcode 决定长度）| 4-byte offset  |
| Block Buffer       | 所有 basic block（纯 instruction offset 序列）| 变长（terminator 界定边界）| 4-byte offset  |
| Function Table     | 所有 function entry                      | 定长                   | index          |

三者都是 append-only，避免 per-element heap allocation。

### 3.1 Instruction Buffer

A single `std::vector<uint8_t>` holds all instructions for the entire program, serialized back-to-back.

```
┌────────────────────────────────────────────────────────────────┐
│ Inst0 │ Inst1 │ Inst2 │ ... │ InstN                           │
└────────────────────────────────────────────────────────────────┘
  offset=0  offset=?  offset=?          each instruction is variable-length
```

Each instruction is encoded as:

```
┌─────────┬──────────┬──────────────────────────┐
│ Opcode  │ TypeId   │ Args...                  │
│ (1 byte)│ (1 byte) │ (4 bytes each)           │
├─────────┴──────────┴──────────────────────────┤
│ Total size = 2 + 4 * numArgs(opcode)  bytes   │
└──────────────────────────────────────────────┘
```

**Opcode 决定指令长度**（论文 §3.2: "an opcode which identifies the instruction — and determines its lengths"）。没有显式的参数数量字段。解码器通过一张静态表 `opcode → arg_count` 即可计算任意指令的字节长度。

对于参数数量可变的指令（`phi`、`call`），使用**按参数数量分化的 opcode 变体**：

| 逻辑指令 | Opcode 变体                   | 参数数量 |
|---------|-------------------------------|---------|
| `phi`   | `phi2`, `phi3`, `phi4`, ...   | 2N (N 个 block-value 对) |
| `call`  | `call0`, `call1`, `call2`, ...| 1 + N (callee + N 个参数) |

这使得每条指令的长度完全由 opcode 决定，解码时无需额外读取长度字段。论文提到共 104 个 opcode，其中包含这些变体。

其他编码细节：
- **Const 指令**：header 之后紧跟常量值的 payload（1/4/8 字节，由 TypeId 决定），无 arg。
- 4-byte offset 允许寻址最多 4GB 的指令——远超单个查询的需求。

### 3.2 Basic Block Buffer

论文 §3.2: "The basic blocks of a program are **similarly stored consecutively in memory**. A basic block contains a dynamic array of instruction offsets which point into the instruction array and determine which instructions are in the basic block and in which order."

所有 basic block 存储在第二个连续的动态数组（block buffer）中。每个 block **只包含 instruction offset 序列，没有任何元信息**（没有 `num_insts`、没有 name、没有 successor 列表）。Block 的边界由 terminator 指令隐式界定——读取时逐个 instruction offset 解码指令，遇到 terminator（`br`/`condbr`/`ret`/`retvoid`/`switch`）即知 block 结束。

```
Block Buffer (std::vector<uint32_t>):
┌──────────────────────────────────────────────────────────┐
│ BB "entry"         │ BB "yes"  │ BB "no"                 │
│ [o0][o1][o2][o3]   │ [o0][o1]  │ [o0][o1]               │
└──────────────────────────────────────────────────────────┘
  ^offset=0            ^offset=16  ^offset=24
  (4 inst offsets)     (2 inst)    (2 inst)
```

Basic block 通过其在 block buffer 中的 **byte offset** 引用（4-byte offset，与 instruction 的引用方式一致）。Terminator 指令中的 branch target 编码的是目标 block 的 byte offset。

不需要存储 block 长度的原因：
- Block 的发现路径是 function → entry block offset → terminator 中的 branch target offset → 下一个 block...
- 读取某个 block 时，从其 offset 开始逐个读 `uint32_t`（instruction offset），对每个 offset 去 instruction buffer 解码指令，直到遇到 terminator 为止
- 因此 block 的长度信息隐含在其最后一条指令（terminator）中

```
两个 buffer 之间的关系:

Block Buffer                       Instruction Buffer
┌───────────────────────┐          ┌────────────────────────────────────┐
│ BB "entry" @offset=0  │          │                                    │
│   [0, 3, 10, 17] ─────────────► │  [FunArg] [Const;5] [CmpGt] [CondBr]
│                       │          │   ^0       ^3       ^10     ^17   │
├───────────────────────┤          │                                    │
│ BB "yes" @offset=16   │          │  [Const;1] [Ret]                   │
│   [24, 29] ────────────────────► │   ^24       ^29                    │
│                       │          │                                    │
├───────────────────────┤          │  [Const;0] [Ret]                   │
│ BB "no" @offset=24    │          │   ^34       ^39                    │
│   [34, 39] ────────────────────►                                      │
└───────────────────────┘          └────────────────────────────────────┘
```

这个设计和 instruction buffer 完全平行：
- 两个 buffer 都是 append-only 的连续内存
- 引用都是 4-byte byte offset
- 都避免了 per-element heap allocation
- Block 不存储任何冗余信息——其长度、successor 全部由 instruction 内容决定

### 3.3 Function Storage

Functions are stored consecutively, but each function only records the **offset of its entry basic block**（论文 §3.2: "Each function, however, only contains the offset of the first basic block"）. All other blocks belonging to the function are discovered by following branch targets from the entry block — there is no explicit block list.

论文中 function 只存一个字段：

```
FunctionEntry (paper-faithful):
    uint32_t entry_block_offset;  // byte offset of the entry basic block in block storage
```

> **[MVP deviation]** MVP 在 FunctionEntry 中附加了 cold metadata，方便 backend 使用和调试。这些字段不在论文的 hot-path 结构描述中：
>
> ```
> FunctionEntry (MVP):
>     uint32_t entry_block_offset;  // paper-faithful
>     uint32_t num_params;          // MVP: backend 需要知道参数数量来处理 FunArg
>     TypeId return_type;           // MVP: backend 需要知道返回类型来 emit ret
>     std::string name;             // MVP: debug/profiling 用（perf jit, GDB）
> ```
>
> 另一种做法是从 entry block 的 `FunArg` 指令中推导 `num_params`，从 `ret` 指令中推导 `return_type`，将 `name` 放在独立的 debug info 结构中。MVP 选择冗余存储以简化实现。

To enumerate all blocks of a function, start from `entry_block_offset` and transitively follow all branch/condbr/switch targets. This is the same traversal backends must perform anyway.

Function parameters are represented as `FunArg` instructions at the beginning of the entry block.

---

## 4. Type System

Umbra IR operates on a small set of primitive types that map directly to machine capabilities.

| TypeId    | Size   | Description                           |
|-----------|--------|---------------------------------------|
| `Void`    | 0      | No value (for stores, branches)       |
| `Bool`    | 1 byte | Boolean (0 or 1)                      |
| `Int8`    | 1 byte | Signed 8-bit integer                  |
| `Int32`   | 4 bytes| Signed 32-bit integer                 |
| `Int64`   | 8 bytes| Signed 64-bit integer                 |
| `UInt64`  | 8 bytes| Unsigned 64-bit integer               |
| `Ptr`     | 8 bytes| Opaque pointer (untyped at IR level)  |

> **[MVP simplification]** 论文 Table 1 列出的完整类型集还包括 `Double`、`Data128`（128-bit 用于 hash 中间值拼接）、以及 `UInt8`/`Int16`/`UInt32` 等宽度变体。MVP 裁剪到 Q1 所需的最小集：去掉 `Double`（无浮点需求，DECIMAL 用 scaled Int64）、去掉 `Data128`（hash 用两次 CRC32 + 位运算代替 128-bit 拼接）、去掉未使用的宽度变体。

Notes:
- There is no floating-point type. SQL `DECIMAL` is implemented as scaled `Int64` in higher layers.
- SQL `DATE` is `Int32` (days since epoch). SQL `VARCHAR` is `Ptr` (pointer to a string struct).
- `Bool` is a first-class type. Comparisons produce `Bool`; conditional branches consume `Bool`.
- Pointer types are untyped in the IR itself (`Ptr`). The element type is tracked by the Codegen API layer (C++ `Ptr<T>` templates) at system compile time, not at query compile time.

---

## 5. Instruction Set

### 5.1 Notation

Textual representation follows a consistent format:

```
%result = opcode type operand1, operand2, ...
```

Examples:
```
%3 = add i32 %1, %2
%5 = load i64 %4
%8 = cmplt i32 %6, %7
     condbr void %8, block2, block3
     ret i32 %1
```

Values are named by their offset: `%0`, `%3`, `%10`, etc. (offsets are not sequential because instructions have variable length.)

### 5.2 Constants and Arguments

| Opcode     | Format                           | Description                          |
|------------|----------------------------------|--------------------------------------|
| `const`    | `%r = const <type> <value>`      | Materialize a constant value         |
| `funarg`   | `%r = funarg <type> <index>`     | Reference the Nth function parameter |

Constants are deduplicated during IR construction (constant folding). If the same constant is appended twice, the builder returns the existing offset.

### 5.3 Arithmetic

All arithmetic instructions are binary: `%r = op <type> %a, %b`

| Opcode       | Semantics             | Notes                                    |
|--------------|-----------------------|------------------------------------------|
| `add`        | `a + b`               |                                          |
| `sub`        | `a - b`               |                                          |
| `mul`        | `a * b`               |                                          |
| `div`        | `a / b`               | Signed division                          |
| `udiv`       | `a / b`               | Unsigned division                        |
| `mod`        | `a % b`               | Signed remainder                         |
| `neg`        | `-a`                  | Unary; encoded as binary with 0 first arg|

#### Checked Arithmetic

| Opcode         | Format                                               | Description                  |
|----------------|------------------------------------------------------|------------------------------|
| `checkedadd`   | `%r = checkedadd <type> %a, %b, %cont, %overflow`    | Add with overflow check      |
| `checkedsub`   | `%r = checkedsub <type> %a, %b, %cont, %overflow`    | Sub with overflow check      |
| `checkedmul`   | `%r = checkedmul <type> %a, %b, %cont, %overflow`    | Mul with overflow check      |

These combine an arithmetic operation with a conditional branch. If no overflow occurs, execution continues at `%cont` with the result; on overflow, execution branches to `%overflow`. This eliminates a separate comparison and branch, allowing backends to use hardware overflow flags directly (e.g., `jo` on x86).

> **[MVP simplification]** 论文中 checked arithmetic 是 Umbra IR 的重要特色指令之一。MVP 先实现 opcode 定义和 interpreter 支持，但 Flying Start 后端初始可以用普通算术 + 单独的 overflow 分支代替，待基础功能稳定后再优化为融合形式。

### 5.4 Bitwise

| Opcode    | Semantics        |
|-----------|------------------|
| `and`     | `a & b`          |
| `or`      | `a \| b`         |
| `xor`     | `a ^ b`          |
| `shl`     | `a << b`         |
| `ashr`    | `a >> b` (arith) |
| `lshr`    | `a >> b` (logic) |
| `rotl`    | Rotate left      |
| `rotr`    | Rotate right     |

> **[MVP simplification]** `rotl`/`rotr` 在论文中用于 hash 计算（§2.5 hash 函数示例中使用了 `rotr i64 %2, 32` 来组合两个 32-bit CRC32 值）。MVP 保留这两个 opcode 因为 hash aggregation 需要它们。但如果实现困难可以先用 `shl` + `lshr` + `or` 三条指令模拟 rotate，后续再加专用 opcode。

### 5.5 Comparison

| Opcode   | Semantics     | Result type |
|----------|---------------|-------------|
| `cmpeq`  | `a == b`      | `Bool`      |
| `cmpne`  | `a != b`      | `Bool`      |
| `cmplt`  | `a < b`       | `Bool`      |
| `cmple`  | `a <= b`      | `Bool`      |
| `cmpgt`  | `a > b`       | `Bool`      |
| `cmpge`  | `a >= b`      | `Bool`      |

All comparisons take two operands of the same type and produce `Bool`. In Flying Start, comparisons can be fused with a subsequent `condbr` to emit a single `cmp` + `jcc` sequence.

### 5.6 Logical

| Opcode   | Semantics     | Notes               |
|----------|---------------|---------------------|
| `lnot`   | `!a`          | Bool → Bool         |
| `land`   | `a && b`      | Bool × Bool → Bool  |
| `lor`    | `a \|\| b`    | Bool × Bool → Bool  |
| `select` | `c ? a : b`   | Bool × T × T → T   |

`select` is a value-level conditional (no control flow). Useful for branchless NULL propagation.

### 5.7 Cast

| Opcode     | Description                               |
|------------|-------------------------------------------|
| `sext`     | Sign-extend to wider type                 |
| `zext`     | Zero-extend to wider type                 |
| `trunc`    | Truncate to narrower type                 |
| `bitcast`  | Reinterpret bits as another type          |
| `ptr2int`  | Pointer to integer                        |
| `int2ptr`  | Integer to pointer                        |

### 5.8 Memory

| Opcode          | Format                                       | Description                          |
|-----------------|----------------------------------------------|--------------------------------------|
| `load`          | `%r = load <type> %ptr`                      | Load value from memory address       |
| `store`         | `store void %val, %ptr`                      | Store value to memory address        |
| `getelementptr` | `%r = getelementptr ptr %base, <type> %idx`  | Compute `base + idx * sizeof(type)`  |

`getelementptr` (GEP) computes an address without accessing memory. It calculates pointer arithmetic: `base + index * element_size`. Multiple GEPs often chain together (e.g., accessing a field within an array element). Flying Start's lazy address calculation optimization delays GEP translation to fold chains into x86 `[base + offset]` addressing modes.

### 5.9 Control Flow

| Opcode    | Format                                   | Description                     |
|-----------|------------------------------------------|---------------------------------|
| `br`      | `br void %target_block`                  | Unconditional branch            |
| `condbr`  | `condbr void %cond, %true_bb, %false_bb` | Conditional branch on Bool     |
| `ret`     | `ret <type> %val`                        | Return from function            |
| `retvoid` | `retvoid void`                           | Return void                     |
| `call0`/`call1`/...| `%r = call <type> %func, %arg0, ...` | Call function (opcode 编码参数数量) |
| `switch`  | `switch void %val, %default, [v0:%bb0, v1:%bb1, ...]` | Multi-way branch  |

Branch targets are encoded as **block buffer byte offsets**（与 §3.2 一致，构建期通过 backpatch 从 block ID 替换为 byte offset，见 §9.1）。`call` encodes the callee as a function index; the call target can be either a JIT-compiled function or a precompiled C++ host function (proxy call).

> **[MVP simplification]** 论文 §3.1 还描述了 `switch` 指令用于多路分支。Q1 不需要 `switch`，但保留 opcode 定义以备后续查询扩展。MVP 阶段 interpreter 和 Flying Start 均可暂不实现 `switch` 的翻译。
>
> 论文 Table 1 列出了 `atomicXchg`、`atomicCmpXchg`、`atomicStore` 等原子操作指令，用于多线程并发执行。MVP 是单线程的，完全去掉所有 atomic 指令。

### 5.10 SSA

| Opcode          | Format                                            | Description                          |
|-----------------|---------------------------------------------------|--------------------------------------|
| `phi2`/`phi3`/...| `%r = phi <type> [%bb0:%v0, %bb1:%v1, ...]`      | SSA phi node (opcode 编码 incoming 数量) |

PHI nodes appear at the beginning of a basic block. Each operand is a `(predecessor_block, value)` pair. At runtime, the PHI selects the value corresponding to the predecessor block from which control arrived. Opcode 变体 `phiN` 表示有 N 个 incoming edge，因此参数数量 = 2N（N 个 block + N 个 value）。

PHI nodes are not constructed manually. The Codegen API's control-flow constructs (`If`, `Loop`) automatically insert PHIs when needed:
- `If` inserts PHIs when a value is defined in both the then and else branches and used after the join point (via `if.phi(a, b)`).
- `Loop` inserts PHIs for loop variables — values that are updated on each iteration (via constructor's initial values and `loop.done(updated_values)`).

### 5.11 DB-Specific Instructions

Custom instructions that express database-specific intent compactly:

| Opcode    | Format                          | Description                                      |
|-----------|---------------------------------|--------------------------------------------------|
| `isnull`  | `%r = isnull bool %val`        | Check if value represents NULL (no 2nd operand)  |
| `crc32`   | `%r = crc32 u64 %data, %seed`  | Hardware CRC32 for hashing                       |
| `bswap`   | `%r = bswap <type> %val`       | Byte swap                                        |

`isnull` avoids the need for a separate NULL constant and comparison. `crc32` maps directly to the x86 `crc32` instruction, critical for hash table performance.

> **[MVP simplification]** 论文 §3.4 还提到了 `getelementptr` 与 `load`/`store` 以 inlining 方式合并成复合指令（类似 CISC 的 load-with-address-calculation），以及 128-bit 数据类型的专用指令。MVP 保持 `getelementptr`、`load`、`store` 各自独立，地址折叠留给 Flying Start 后端的 lazy address calculation 优化处理，不在 IR 层面合并。

---

## 6. IR Builder

The IR builder is the programmatic interface through which the Codegen API layer (and thus all upper Tidy Tuples layers) emits instructions.

### 6.1 Core API

```
IRBuilder:
    // Program structure
    createFunction(name, returnType, paramTypes) → FunctionId
    createBlock(function) → BlockId
    setInsertPoint(block)
    getCurrentBlock() → BlockId

    // Instructions (return IROp = offset of the created instruction)
    emitConst(type, value) → IROp
    emitAdd(type, lhs, rhs) → IROp
    emitSub(type, lhs, rhs) → IROp
    emitMul(type, lhs, rhs) → IROp
    ...
    emitLoad(type, ptr) → IROp
    emitStore(val, ptr)
    emitGEP(base, type, index) → IROp
    emitCall(func, args...) → IROp    // selects call0/call1/... variant by args count
    emitBr(targetBlock)
    emitCondBr(cond, trueBlock, falseBlock)
    emitRet(val)
    emitPhi(type, incomingList) → IROp // selects phi2/phi3/... variant by incoming count

    // Finalize
    finalize() → Program
```

> **[MVP simplification]** 论文 §2.6 描述了 proxy 系统：在 C++ 系统编译时自动为预定义的类和函数生成代理，使得生成代码可以无缝调用预编译的 C++ 函数，并且可以标记函数为 inline（Flying Start 忽略 inline 标记，LLVM 后端会内联）。MVP 简化为：`call` 指令通过函数指针直接调用预编译的 C++ 函数（如 hash table 的 insert/lookup），不实现自动 proxy 生成系统。调用约定手动处理。

### 6.2 Constant Folding

The builder performs constant folding at instruction creation time:

- `emitAdd(const(3), const(4))` → returns `const(7)` without emitting an `add`
- `emitMul(x, const(1))` → returns `x`
- `emitMul(x, const(0))` → returns `const(0)`
- `emitCondBr(const(true), bb1, bb2)` → emits `br bb1`

Constants are deduplicated: creating `const i32 5` twice returns the same offset.

### 6.3 IROp Handle

`IROp` is a lightweight value type (4-byte offset) that represents an SSA value. It is the currency passed between all Tidy Tuples layers:

```
Codegen API:  IROp a = builder.emitLoad(Int64, ptr);
              IROp b = builder.emitConst(Int64, 1);
              IROp c = builder.emitAdd(Int64, a, b);
                       builder.emitStore(c, ptr);
```

The offset-based representation means that `IROp` values are trivially copyable, comparable, and hashable, with no indirection.

---

## 7. On-the-fly Optimizations

The IR itself only performs optimizations during construction (constant folding) and one post-construction pass (dead code elimination). All other optimizations happen in the backends.

### 7.1 Constant Folding (during construction)

Evaluated at `emit*` time. Covers:
- Arithmetic on two constants
- Identity operations (add 0, mul 1, and -1, or 0)
- Absorbing operations (mul 0, and 0)
- Comparison of two constants
- Conditional branch with constant condition

### 7.2 Dead Code Elimination (post-construction)

A single reverse pass over the instruction buffer:
1. Mark all instructions as dead.
2. Walk terminators of all reachable blocks; mark them live.
3. Propagate liveness: if an instruction is live, mark all its operands as live.
4. Remove dead instructions (mark as `nop` or compact the buffer).

Also removes unreachable basic blocks — blocks with no predecessor (except the entry block).

DCE is important because the Tidy Tuples layers sometimes emit code speculatively (e.g., both branches of an `If` even when one is dead). This keeps the upper layers simple; DCE cleans up afterward.

> **[MVP simplification]** 论文 §3.3 的 DCE 描述与我们一致——这是 Umbra IR 仅有的两个优化之一（另一个是 constant folding）。MVP 完整实现这两个优化，不做裁剪，因为它们对 codegen 正确性和代码质量都很重要，且实现成本低。

---

## 8. Textual Representation

For EXPLAIN and debugging, the IR can be printed in a human-readable text format:

```
define i32 @pipeline_0(ptr %0) {
entry:
    %1 = const i64 0
    %2 = const i64 60000
    %3 = const i32 19980902
    br void loop_header

loop_header:
    %4 = phi i64 [entry: %1, loop_body: %12]
    %5 = cmplt i64 %4, %2
    condbr void %5, loop_body, loop_end

loop_body:
    ; load l_shipdate
    %6 = getelementptr ptr %0, i64 %4     ; base + row_idx
    %7 = load i32 %6
    ; filter: l_shipdate <= 19980902
    %8 = cmple i32 %7, %3
    condbr void %8, process_row, loop_continue

process_row:
    ; ... aggregate logic ...
    br void loop_continue

loop_continue:
    %12 = add i64 %4, const(1)
    br void loop_header

loop_end:
    retvoid void
}
```

Conventions:
- Type prefixes: `i8`, `i32`, `i64`, `u64`, `bool`, `ptr`, `void`
- Functions: `define <ret_type> @<name>(<params>)`
- Blocks: `<name>:` label on its own line
- Values: `%<offset>` (decimal offset in instruction buffer)
- Constants printed inline when small: `const(1)`, `const(0x3F800000)`

---

## 9. Serialization & Iteration

### 9.1 Writing (Code Generation → IR)

构建 IR 涉及两个 buffer 的协同写入和一次 finalize。

**构建期**，branch target 暂时编码为 **block ID**（临时整数），而非 byte offset。Builder 内部维护一个临时映射 `block_id → block buffer byte offset`，在 block 开始写入时填充。

```
createBlock():
    1. block_id = next_block_id++
    2. Return block_id                    // offset 尚未确定

beginBlock(block_id):
    1. block_offset = block_buffer.size() // 此刻确定 offset
    2. block_offsets[block_id] = block_offset
    3. current_block = block_id

emitInstruction(opcode, args...):
    1. Determine opcode variant (e.g., call2 for 2-arg call)
    2. inst_offset = instruction_buffer.size()
    3. Append [opcode, type, args...] to instruction_buffer
    4. Append inst_offset (as uint32_t) to block_buffer
    5. Return inst_offset as IROp
    // terminator 写入后 block 自然结束

emitCondBr(cond, true_block_id, false_block_id):
    // branch target 暂时写入 block_id（占位）
    // 记录到 patch_list 供 finalize 替换
```

**Finalize** 一次遍历 instruction buffer，将所有 terminator 指令中的 block ID 替换为实际 byte offset：

```
finalize():
    for each (inst_offset, arg_index) in patch_list:
        block_id = read_u32(instruction_buffer, inst_offset + 2 + 4*arg_index)
        actual_offset = block_offsets[block_id]
        write_u32(instruction_buffer, inst_offset + 2 + 4*arg_index, actual_offset)
    // PHI 的 block 参数同样替换
    // 完成后 block_offsets 和 patch_list 可丢弃，不进入最终 IR
```

两个 buffer 都是 append-only，通过 doubling 增长，无 per-element heap allocation。Finalize 是 O(terminator 数量) 的单次遍历，开销可忽略。

> 同理，`Loop` 构造中 PHI 的 back-edge value（指向尚未生成的 loop body 中的指令）也用相同的 backpatch 机制处理：PHI 先写入占位 arg，`loop.done()` 时回填实际 instruction offset。

### 9.2 Reading (IR → Backend)

Finalize 之后，IR 中所有 branch target 都是 block buffer 的 byte offset，无需任何间接表。

Backends 从 function 的 `entry_block_offset` 出发，在 block buffer 中逐个读取 instruction offset，对每条指令解码，遇到 terminator 即知 block 结束。Terminator 中的 branch target（已是 byte offset）指向下一批 block。

```
read flow:
    for each function f:
        worklist = {f.entry_block_offset}
        visited = {}
        while worklist not empty:
            block_off = worklist.pop()
            if block_off in visited: continue
            visited.add(block_off)

            pos = block_off
            loop:
                inst_off = read_u32(block_buffer, pos)
                pos += 4

                opcode = inst_buffer[inst_off]
                type   = inst_buffer[inst_off + 1]
                nargs  = argCount[opcode]
                args   = &inst_buffer[inst_off + 2]
                // translate or interpret this instruction

                if isTerminator(opcode):
                    // extract branch target byte offsets, add to worklist
                    break
```

读取时无需 block offset table——branch target 直接就是 byte offset。逐条解码指令检查 terminator 不构成额外开销，因为 backend 无论如何都要读取每条指令的 opcode 来翻译/解释执行。

### 9.3 Instruction Accessor

A lightweight view type for reading an instruction without copying:

```
InstructionView:
    opcode() → Opcode
    type() → TypeId
    numArgs() → uint8_t    // from static table argCount[opcode]
    arg(i) → IROp          // the offset of the i-th operand, at buffer[offset + 2 + 4*i]
    offset() → uint32_t    // this instruction's own offset (= its IROp)
    byteSize() → uint32_t  // 2 + 4 * numArgs() (+ payload for const)
```

This is a zero-copy view into the instruction buffer. `numArgs()` is not read from the buffer but derived from a static `opcode → arg_count` table.

---

## 10. Example: TPC-H Q1 Filter Expression

The SQL expression `l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY` with `l_shipdate` as `Int32` (days since epoch).

The date `1998-09-02` is precomputed to the integer `-90 + days_since_epoch('1998-12-01')` during binding. So the filter becomes a simple integer comparison.

Generated IR (within the scan loop body):

```
; %row_ptr is the current row's l_shipdate column pointer
; %threshold is const i32 <precomputed_date_value>

%shipdate = load i32 %row_ptr
%passes   = cmple i32 %shipdate, %threshold
             condbr void %passes, %aggregate_block, %next_row
```

This produces exactly 3 instructions for the filter — demonstrating how higher-level SQL semantics collapse into minimal IR.

---

## 11. Example: Hash Aggregate Update

For the aggregate expression `SUM(l_extendedprice * (1 - l_discount))`:

```
; %price_ptr, %discount_ptr: pointers to column values for current row
; %entry_ptr: pointer to the hash table entry's aggregate slot

%price    = load i64 %price_ptr          ; scaled decimal
%discount = load i64 %discount_ptr       ; scaled decimal
%one      = const i64 100                ; 1.00 in scaled representation
%factor   = sub i64 %one, %discount      ; (1 - l_discount)
%product  = mul i64 %price, %factor      ; l_extendedprice * (1 - l_discount)
%product_scaled = div i64 %product, %one ; rescale after multiplication

; accumulate into hash table entry
%old_sum  = load i64 %entry_ptr
%new_sum  = add i64 %old_sum, %product_scaled
             store void %new_sum, %entry_ptr
```

---

## 12. Design Decisions Summary

| Decision                              | Rationale                                                        |
|---------------------------------------|------------------------------------------------------------------|
| Opcode 决定指令长度（无 NumArgs 字段） | 每条指令省 1 字节；解码器是纯静态表查找，无分支 |
| 变参指令用 opcode 变体（phi2/call3 等）  | 保持"opcode 决定长度"不变；opcode 空间充足（1 字节 = 256 个） |
| 4-byte offsets instead of pointers    | Half the size; sufficient for any single-query program            |
| Contiguous instruction byte buffer    | Cache-friendly append; minimal allocations                       |
| Block holds offset array, not slice   | Instruction buffer stays append-only; blocks can interleave during codegen (e.g., If/Loop constructs) without moving instructions |
| Function stores entry block only      | Other blocks discovered via branches; no redundant block list to maintain |
| SSA form generated directly           | No extra mem2reg pass; Codegen API handles PHI insertion          |
| Constant folding at build time        | Reduces program size before backends ever see it                 |
| Single DCE pass only                  | Low compile-time cost; cleans up speculative codegen              |
| No instruction reordering             | Append-only buffer; reordering would be expensive                |
| DB-specific opcodes (crc32, isnull)   | Express intent; backends can emit optimal machine code            |
| Untyped pointers in IR               | Simpler IR; type safety enforced by C++ Codegen API at system compile time |

---

## 13. MVP Simplifications Summary

> 本节汇总所有相对于论文的简化，方便评估 MVP 与完整 Umbra 的差距。

| 论文特性 | MVP 处理 | 影响 |
|---------|---------|------|
| `Double` 类型 | 去掉 | Q1 不涉及浮点；DECIMAL 用 scaled Int64 |
| `Data128` 类型 | 去掉 | hash 用两次 CRC32 + 位运算代替 128-bit 拼接 |
| `UInt8`/`Int16`/`UInt32` 等宽度变体 | 去掉 | Q1 仅需 Int8(Bool 底层)/Int32/Int64/UInt64 |
| `atomicXchg`/`atomicCmpXchg`/`atomicStore` | 完全去掉 | MVP 单线程，无并发需求 |
| `switch` 指令 | 保留 opcode 定义，不实现翻译 | Q1 不需要多路分支 |
| Checked arithmetic 的 Flying Start 融合翻译 | 先用普通算术 + 单独 overflow 分支 | 功能正确，性能略有损失；后续再优化为融合 `cmp`+`jo` |
| `getelementptr` 与 `load`/`store` 的 IR 层合并 | 保持独立指令 | 地址折叠由 Flying Start 的 lazy address calculation 负责 |
| Proxy 系统（自动生成 C++ 函数代理） | 手动 `call` 函数指针 | 不影响正确性；codegen 层需手动处理调用约定 |
| Constant folding + DCE | 完整实现，不裁剪 | 与论文一致 |
| 物理布局（block 持有 offset 数组 / function 只存 entry block） | 完整实现，不裁剪 | 与论文一致 |
