# Flying Start Design Document

## 1. Overview

Flying Start 是 Umbra IR 的低延迟编译后端，使用 asmjit 的 `x86::Assembler` 将 IR 直接翻译为 x86-64 机器码。目标是**最小化编译时间 + 运行时间的总和**。

```
Umbra IR Program
     │
     ├── Pass 0: Live Span Analysis (reverse post-order, +1 pass)
     │
     └── Pass 1: Direct Translation (single pass over blocks)
              │
              │  on-the-fly optimizations:
              │  ├── Stack Space Reuse
              │  ├── Register Allocation (heuristic)
              │  ├── Lazy Address Calculation
              │  └── Fuse Comparison and Branch
              │
              ▼
         x86-64 Machine Code (in asmjit CodeHolder)
              │
              ▼
         Executable Function Pointer
```

### 设计原则

1. **最多两次遍历**：一次 live span 分析 + 一次翻译，总共最多 2 pass
2. **无中间数据结构**：不构建 CFG、不构建 interference graph，优化嵌入在翻译过程中
3. **每条 IR 指令 → 固定的 x86 指令序列**：无指令选择搜索，翻译函数是确定性的

---

## 2. Compilation Pipeline

### 2.1 Input

一个 finalized 的 IR Program（见 doc/ir.md）。Branch target 已是 block buffer byte offset，所有 backpatch 已完成。

### 2.2 Block Discovery

从 function 的 `entry_block_offset` 出发，用 worklist 发现所有可达 block（见 doc/ir.md §9.2）。为每个 block 分配一个 asmjit `Label`，用于 branch 目标绑定。

```
block_labels: map<uint32_t, asmjit::Label>   // block_buffer byte offset → label

discoverBlocks(function):
    worklist = {function.entry_block_offset}
    while worklist not empty:
        block_off = worklist.pop()
        if block_off in block_labels: continue
        block_labels[block_off] = assembler.newLabel()
        // 解析 block_buffer 得到 BlockView，找到 terminator
        view = parseBlock(block_buffer, inst_buffer, block_off)
        terminator = decodeInst(inst_buffer, view.inst_offsets.back())
        for each branch_target_offset in terminator.args:
            worklist.push(branch_target_offset)
```

### 2.3 Block Ordering

翻译顺序影响 branch 距离和 cache 行为。使用 **reverse post-order (RPO)** 遍历——这是编译器的标准做法，保证在翻译某个 block 时其所有 dominator 已被翻译。

RPO 同时用于 live span analysis（§3）和实际翻译（§4）。

---

## 3. Live Span Analysis (Pass 0)

在翻译之前，需要知道每个值的 **live span**（从定义点到最后使用点）以支持 stack space reuse 和 register allocation。

### 3.1 定义

一个值的 live span = `[def_point, last_use_point]`，其中 point 是指令在翻译顺序中的序号。

### 3.2 计算

在 RPO 顺序下遍历所有 block 的所有指令，为每条指令分配一个递增的序号。对每个值（instruction offset），记录其定义序号和最后使用序号：

```
computeLiveSpans(program):
    seq = 0
    for block_off in RPO_order:          // block_off = block buffer byte offset
        // 解析 block_buffer 得到 BlockView（见 doc/ir.md §3.2）
        view = parseBlock(block_buffer, inst_buffer, block_off)
        for inst_off in view.inst_offsets:
            def_point[inst_off] = seq
            inst = decodeInst(inst_buffer, inst_off)
            for arg in inst.args:
                last_use[arg] = seq      // 更新 arg 的最后使用点
            seq++

    // live_span[v] = (def_point[v], last_use[v])
```

> `parseBlock` 从 block_buffer 的指定 offset 开始，逐个读取 `uint32_t` instruction offset 并解码指令，直到遇到 terminator，返回一个轻量 `BlockView{block_off, inst_offsets[]}`。这个 view 是临时的，不持久化。

这是线性时间的单次遍历。

> 论文 §4.3: "live spans can be calculated in linear time and require little memory space per value, which makes them ideal for fast compilation."

### 3.3 特殊处理

- **PHI 节点**：PHI 的值在 header block 定义，但其 incoming value 来自不同的 predecessor。PHI 的 live span 从 header 到其最后使用点。Incoming value 的 live span 需要延伸到包含 PHI 的 block 的 predecessor 的末尾。
- **Loop 变量**：loop 内定义的值如果被 PHI 引用（back-edge），其 live span 需要延伸到 loop 末尾（而非最后显式使用点）。

---

## 4. Direct Translation (Pass 1)

按 RPO 顺序遍历每个 block 的每条指令，为每条 IR 指令 emit 一段 x86-64 指令。

### 4.1 Value Location Model

每个 IR 值在翻译后存在于以下位置之一：

| 位置 | 表示 | 访问成本 |
|------|------|---------|
| Machine Register | `rax`, `rbx`, ... | 0 cycles |
| Stack Slot | `[rsp + offset]` | ~4 cycles (L1 hit) |
| Flags Register | CPU flags | 0 cycles（仅 comparison 结果）|
| Deferred | 尚未翻译 | — |

`Reg` 类跟踪每个值的位置和引用计数。

### 4.2 Basic Translation Pattern

以 `add i64 %a, %b` 为例，最基本的翻译（无优化）：

```x86
mov  rax, [rsp + slotA]    ; load first operand from stack
add  rax, [rsp + slotB]    ; add second operand
mov  [rsp + slotResult], rax ; store result to stack
```

优化后（两个操作数都在寄存器中）：

```x86
add  r9, r10               ; 直接寄存器操作，result 留在 r9
```

### 4.3 Translation Dispatch

```
translateBlock(block_off):    // block_off = block buffer byte offset
    assembler.bind(block_labels[block_off])

    view = parseBlock(block_buffer, inst_buffer, block_off)
    for inst_off in view.inst_offsets:
        inst = decodeInst(inst_buffer, inst_off)
        switch (inst.opcode):
            case Add:    translateAdd(inst)
            case Sub:    translateSub(inst)
            case Load:   translateLoad(inst)
            case Store:  translateStore(inst)
            case CondBr: translateCondBr(inst)
            case Phi:    translatePhi(inst)
            case Call:   translateCall(inst)
            // ...
```

---

## 5. On-the-fly Optimizations

四个优化嵌入翻译过程中，由 `Reg` 和 `ScratchReg` 基础设施驱动。

### 5.1 Stack Space Reuse

利用 live span 信息复用 stack slot。当一个值的 live span 结束后（翻译完其最后使用者），其 stack slot 归还给分配器。

```
translateInstruction(inst):
    // ... emit x86 code ...

    // 检查每个 operand：如果这是其最后使用点，释放 slot
    for arg in inst.args:
        if current_seq == last_use[arg]:
            freeStackSlot(arg)
```

效果：减少 stack 帧大小，提高 cache 利用率。论文数据：增加 ~45% 编译时间（因为需要 live span pass），但使后续优化成为可能。

### 5.2 Register Allocation

启发式寄存器分配，在翻译每条指令时决定结果放在寄存器还是 stack。

**可用寄存器**（System V AMD64 ABI）：

> **[MVP deviation]** 论文 §4.4 的预算是：16 个通用寄存器 - 4 scratch - 1 stack pointer = **11 个**可分配寄存器。MVP 额外保留了 RBP（frame pointer，简化 stack 管理和调试）和 RDI（固定为 RuntimeContext 指针），得到 **9 个**。这是合理的实现简化——RBP 和 RDI 的保留换来了更简单的 prologue/epilogue 和 host function 调用。

| 类别 | 寄存器 | 数量 |
|------|--------|------|
| Callee-saved（用于长期值）| RBX, R12-R15 | 5 |
| Caller-saved（用于临时值）| R8-R11 | 4 |
| Scratch（翻译单条指令内部用）| RAX, RCX, RDX, RSI | 4 |
| Reserved | RSP (stack), RBP (frame, MVP), RDI (runtime ctx, MVP) | 3 |
| **可分配总数** | | **9** (论文: 11) |

**分配启发式**（论文 §4.4, Figure 10 `resultReg`）：

```
resultReg(value):
    if value is constant:
        return ConstLocation       // 不分配，常量内联到指令中

    if registersAvailable() &&
       (onlyLiveInCurrentBlock(value) || loopIsDeepestNest()):
        reg = allocateRegister()
        return RegisterLocation(reg)
    else:
        slot = allocateStackSlot(value)
        return StackLocation(slot)
```

核心思想：将寄存器分配给**当前 block 内短命的值**或**最深嵌套循环中的值**。前者因为生命周期短不会占用寄存器太久；后者因为在热循环中，寄存器带来的收益最大。

效果：论文数据显示寄存器分配减少 ~32% 执行时间（最大的单项优化）。

### 5.3 Lazy Address Calculation

`getelementptr` 指令不立即翻译，而是记录 `(base, offset)` 对。当后续 `load`/`store` 使用该地址时，将地址计算折叠到 x86 的寻址模式中。

IR:
```
%ptr1 = getelementptr ptr %base, i32 %idx      ; base + idx * 4
%ptr2 = getelementptr ptr %ptr1, i32 24         ; + 24
%val  = load i64 %ptr2
```

无优化 → 3 条指令:
```x86
lea  rax, [rdx + rcx*4]     ; ptr1
add  rax, 24                ; ptr2
mov  rax, [rax]             ; load
```

有优化 → 1 条指令:
```x86
mov  rax, [rdx + rcx*4 + 24]   ; 直接用 x86 复合寻址模式
```

实现：`argumentReg` 在获取 GEP 的结果时，不返回寄存器值，而是返回一个 `AddressLocation{base_reg, index_reg, scale, displacement}` 结构。`load`/`store` 的翻译函数识别这种 location 并直接 emit 复合寻址。

### 5.4 Fuse Comparison and Branch

比较指令不立即翻译——其翻译被**推迟**。当后续 `condbr` 消费比较结果时，一次性 emit `cmp` + `jcc`。

IR:
```
%5 = cmple i32 %3, %4
     condbr void %5, blockA, blockB
```

无优化 → 4 条指令:
```x86
cmp  r9, r10
setle r13b                 ; 比较结果 → 寄存器
cmp  r13b, 1               ; 再次比较
jne  blockB                ; 分支
```

有优化 → 2 条指令:
```x86
cmp  r9, r10
jle  blockA                ; 直接用 flags
; fall-through to blockB
```

实现：`argumentReg` 在获取 comparison 的结果时，检查该 comparison 是否尚未翻译且只有一个 consumer。如果是，标记为 deferred，返回 `FlagsLocation{condition_code}`。`translateCondBr` 识别这种 location，先翻译 comparison（仅 emit `cmp`，不 emit `setcc`），然后直接 emit `jcc`。

---

## 6. Specific Translation Rules

### 6.1 Arithmetic

| IR | x86 | Notes |
|----|-----|-------|
| `add i64 %a, %b` | `add reg, reg/mem` | 如果 a 在 reg，b 可以是 mem operand |
| `sub i64 %a, %b` | `sub reg, reg/mem` | |
| `mul i64 %a, %b` | `imul reg, reg/mem` | 使用三操作数形式避免覆盖 |
| `div i64 %a, %b` | `mov rax, a; cqo; idiv b` | 必须使用 RAX:RDX，需要保存/恢复 |
| `mod i64 %a, %b` | 同 div，结果取 RDX | |
| `neg i64 %a` | `neg reg` | |

### 6.2 Comparison

| IR | x86 |
|----|-----|
| `cmpeq` | `cmp a, b` + `sete` (或 deferred) |
| `cmplt` | `cmp a, b` + `setl` (或 deferred) |
| `cmple` | `cmp a, b` + `setle` (或 deferred) |

如果被 `condbr` 消费且只有一个 user → deferred（见 §5.4）。

### 6.3 Memory

| IR | x86 | Notes |
|----|-----|-------|
| `load i32 %ptr` | `mov eax, [reg]` | 或复合寻址 `[base + idx*scale + disp]` |
| `load i64 %ptr` | `mov rax, [reg]` | |
| `store %val, %ptr` | `mov [reg], val` | |
| `getelementptr` | deferred → 折叠到 load/store 的寻址模式 | 见 §5.3 |

### 6.4 Control Flow

**`br`**: `jmp label`

**`condbr`**:
- 如果 condition 是 deferred comparison → `cmp` + `jcc`（见 §5.4）
- 否则 → `cmp reg, 1` + `je true_label` + fall-through to false

**`call`**:
1. 将参数放入 System V ABI 寄存器（RDI, RSI, RDX, RCX, R8, R9）
2. Spill 所有 caller-saved 寄存器中的活跃值到 stack
3. 确保 RSP 16-byte 对齐
4. `call <function_address>`
5. 结果在 RAX
6. Restore spilled 值

**`ret`**: `mov rax, val` + `ret`（先 restore callee-saved 寄存器）

### 6.5 PHI

PHI 不直接生成代码。在翻译 predecessor block 的 terminator（`br`/`condbr`）之前，将 PHI 的 incoming value 移动到 PHI 的目标 location（寄存器或 stack slot）。

```
translateBr(inst):
    target_block = inst.arg(0)
    // resolve PHIs in target block
    for each phi in target_block (at beginning):
        src = phi.incomingValueFor(current_block)
        dst = phi.resultLocation
        emit mov(dst, src)
    emit jmp(block_labels[target_block])
```

需要注意 PHI 的 parallel assignment 语义——多个 PHI 可能互相依赖。如果出现 swap 情况（a→b, b→a），使用 scratch register 打破循环。

### 6.6 CRC32

直接映射到硬件指令：

```
IR:  %r = crc32 u64 %data, %seed
x86: crc32 reg_seed, reg_data        ; Intel SSE4.2
```

### 6.7 Constants

小常量（fits in imm32）内联到指令中：

```
IR:  %1 = add i64 %0, const(1)
x86: add r9, 1                        ; immediate operand
```

大常量（64-bit）使用 `mov reg, imm64`。

---

## 7. Function Prologue / Epilogue

### Prologue
```x86
push rbp
mov  rbp, rsp
sub  rsp, <frame_size>        ; 为 stack slots 分配空间
push rbx                      ; 保存 callee-saved registers (按需)
push r12
push r13
push r14
push r15
mov  <reg>, rdi               ; 保存第一个参数 (RuntimeContext*)
```

`frame_size` 在翻译完成后确定（基于 stack slot 分配器的高水位），通过 backpatch 填入。

### Epilogue
```x86
pop  r15
pop  r14
pop  r13
pop  r12
pop  rbx
mov  rsp, rbp
pop  rbp
ret
```

---

## 8. Reg Bookkeeping Class

整个翻译过程的核心基础设施。论文 Figure 9 & 10。

### 8.1 职责

`Reg` 封装一个 IR 值在翻译后的位置和生命周期管理：

```
Reg:
    IRType type              // 值的 IR 类型
    IROp value               // 对应的 IR 值（instruction offset）
    Location location        // 当前位置: Register / Stack / Flags / Deferred / Const
    int refcount             // 使用计数（= 该值的剩余 consumer 数量）

    ~Reg():
        if (--refcount == 0):
            freeResources()  // 归还 register 或 stack slot 给分配器
```

### 8.2 关键函数

**`resultReg(value)`**：翻译指令时调用，决定结果放在哪里（见 §5.2 启发式）。

**`argumentReg(value, hint)`**：获取操作数时调用。

```
argumentReg(value, hint = None):
    loc = lookupLocation(value)

    if loc is Deferred:
        // on-demand 翻译（见 §5.4, §5.3）
        translate(value, hint)     // hint 可以是 "place in flags"
        loc = lookupLocation(value)

    if hint == Flags && loc is not Flags:
        // caller 希望结果在 flags（如 condbr 消费 comparison）
        // 需要重新翻译或 emit cmp

    return Reg(type, value, loc)
```

### 8.3 ScratchReg

翻译单条指令时，如果需要临时寄存器（如 `div` 必须用 RAX），使用 `ScratchReg`：

```
ScratchReg:
    ScratchReg(compiler):
        reg = compiler.allocateScratchRegister()   // RAX/RCX/RDX/RSI
        // 如果 scratch reg 当前持有活跃值，spill 到 stack
    ~ScratchReg():
        compiler.freeScratchRegister(reg)
```

---

## 9. JIT Output

### 9.1 asmjit Usage

```cpp
asmjit::JitRuntime runtime;
asmjit::CodeHolder code;
code.init(runtime.environment());
asmjit::x86::Assembler assembler(&code);

// ... translation emits into assembler ...

void* funcPtr;
runtime.add(&funcPtr, &code);

// 调用
using PipelineFunc = void(*)(RuntimeContext*);
auto pipeline = reinterpret_cast<PipelineFunc>(funcPtr);
pipeline(&ctx);
```

### 9.2 Debug: GDB JIT Interface

编译后通过 `__jit_debug_register_code()` 注册，使 GDB 能在 JIT 代码中设断点。

### 9.3 Profiling: perf jit

写入 `/tmp/perf-<pid>.map`（格式: `<addr> <size> <name>`），使 `perf report` 能将 samples 归因到 pipeline 函数。

---

## 10. MVP Simplifications Summary

| 论文特性 | MVP 处理 | 影响 |
|---------|---------|------|
| 4 项 on-the-fly 优化全部实现 | **分阶段实现** | 见下 |
| Phase 1: 无优化直接翻译 | 所有值在 stack，最基本翻译 | 功能正确但慢 |
| Phase 2: + Stack Space Reuse | 需要 live span pass | 减少 stack 帧 |
| Phase 3: + Register Allocation | 最大性能提升项（-32%） | 接近目标性能 |
| Phase 4: + Lazy Address + Fuse CmpBr | 锦上添花 | 完成全部优化 |
| Checked arithmetic 融合翻译 | 先用普通 add + jo 分开 emit | 功能正确，性能差异小 |
| LLVM 优化后端 | 不实现 | MVP 只用 Flying Start |
| Adaptive execution（运行中切换后端）| 不实现 | MVP 启动时选择 backend |
