# Interpreter Design Document

## 1. Overview

Interpreter 是 Umbra IR 的解释执行后端——逐条读取 IR 指令，直接求值，不生成机器码。

> **[MVP deviation]** Interpreter 不是论文的设计。论文的两个后端是 Flying Start（低延迟 x86 直接生成）和 LLVM optimizing backend。MVP 引入 Interpreter 作为**调试和正确性验证的参考实现**：它实现简单、容易调试，用于与 Flying Start 交叉验证（见 doc/testing.md §3）。

```
Umbra IR Program
     │
     ▼
Interpreter
  ├── Block Discovery (same as Flying Start)
  ├── 逐 block、逐 instruction 解释执行
  ├── Value Slots: flat array indexed by instruction offset
  └── 结果返回
```

### 设计原则

1. **正确性优先**：代码尽可能直白，不做任何优化
2. **与 Flying Start 共享 IR 读取逻辑**：block discovery、instruction decode 复用同一套代码
3. **零编译开销**：直接解释，不需要任何编译时间

---

## 2. Value Storage

每个 IR 值（由 instruction offset 标识）在解释执行时需要一个存储位置。使用一个 flat array，以 instruction offset 为 index。

### 2.1 Value Slot

所有值统一存储为 64-bit。实现中直接使用 `uint64_t`（不使用 struct wrapper），在每个 opcode 的 case 中内联做类型转换（`static_cast<int32_t>` 等），保持代码简单直观：

```cpp
// 实现使用 raw uint64_t，每个 opcode case 内联转换
std::vector<uint64_t> slots_;
```

### 2.2 Slot Array

```cpp
// 按 instruction buffer 大小分配（每个 byte offset 对应一个潜在的值）
// 实际只有指令起始 offset 处的 slot 被使用
slots_.assign(program.instBuffer().size(), 0);
```

这比按指令数量分配浪费一些内存（因为指令是变长的，有些 offset 不是指令起始），但换来 O(1) 的 offset → slot 查找，无需额外的映射。

对 Q1 的 IR 程序，instruction buffer 通常几 KB，所以 slot array 也只有几 KB——内存开销可忽略。

---

## 3. Execution Model

### 3.1 顶层入口

```cpp
uint64_t Interpreter::execute(Program& program, FunctionId func,
                               std::span<uint64_t> args) {
    FunctionEntry& fn = program.functions[func];

    // 1. 分配 value slots
    slots.resize(program.inst_buffer.size());

    // 2. 初始化函数参数
    //    FunArg 指令在 entry block 开头，按序对应参数
    initFunArgs(fn, args);

    // 3. 从 entry block 开始执行
    return executeFrom(program, fn.entry_block_offset);
}
```

### 3.2 Block 执行循环

主循环维护两个状态：`block_off`（当前 block 在 block buffer 中的 byte offset）和 `prev_block_off`（上一个 block 的 offset，用于 PHI resolution）。

进入每个 block 的执行分为两步：
1. **Step 1**: 调用 `resolvePhis` 处理 block 开头的所有 PHI 指令（§3.3）
2. **Step 2**: 逐条执行剩余的非 PHI 指令，直到遇到 terminator

这两步严格分离——主循环不会对 PHI 调用 `executeInst`，PHI 只由 `resolvePhis` 处理。

```cpp
uint64_t Interpreter::executeFrom(Program& program, uint32_t block_off) {
    uint32_t prev_block_off = INVALID;   // 首次进入 entry block 无 predecessor

    while (true) {
        // Step 1: resolve PHIs, 返回第一条非 PHI 指令的 pos
        uint32_t pos = resolvePhis(program, block_off, prev_block_off);

        // Step 2: execute non-PHI instructions
        while (true) {
            uint32_t inst_off = readU32(program.block_buffer, pos);
            pos += 4;

            auto inst = decodeInst(program.inst_buffer, inst_off);
            assert(!isPhi(inst.opcode));   // PHI 已被 Step 1 消费

            ExecuteResult result = executeInst(inst, inst_off);

            if (result.type == CONTINUE) {
                continue;
            }
            if (result.type == BRANCH) {
                prev_block_off = block_off;              // 记录来源
                block_off = result.target_block_off;     // 跳转目标
                break;
            }
            if (result.type == RETURN) {
                return result.return_value;
            }
        }
    }
}
```

`ExecuteResult` 是一个 tagged union：

```cpp
struct ExecuteResult {
    enum Type { CONTINUE, BRANCH, RETURN };
    Type type;
    uint32_t target_block_off;  // for BRANCH
    uint64_t return_value;      // for RETURN
};
```

### 3.3 PHI Resolution

`resolvePhis` 在进入 block 时被调用，处理 block 开头的所有 PHI 指令，返回第一条非 PHI 指令在 block_buffer 中的 pos。

**Parallel semantics**: 多个 PHI 可能互相引用（如 swap：a←b, b←a）。必须先读出所有 PHI 的输入值到临时数组，再批量写入 slots，避免写入顺序影响结果。

```cpp
uint32_t Interpreter::resolvePhis(Program& program, uint32_t block_off,
                                   uint32_t prev_block_off) {
    uint32_t pos = block_off;

    // 如果没有 predecessor（entry block 首次进入），跳过所有 PHI
    if (prev_block_off == INVALID) {
        while (true) {
            uint32_t inst_off = readU32(program.block_buffer, pos);
            auto inst = decodeInst(program.inst_buffer, inst_off);
            if (!isPhi(inst.opcode)) return pos;
            pos += 4;
        }
    }

    // Phase 1: 收集所有 PHI 的 (dst_slot, src_value)，不写入 slots
    std::vector<std::pair<uint32_t, ValueSlot>> phi_results;
    while (true) {
        uint32_t inst_off = readU32(program.block_buffer, pos);
        auto inst = decodeInst(program.inst_buffer, inst_off);
        if (!isPhi(inst.opcode)) break;

        for (int i = 0; i < phiIncomingCount(inst.opcode); i++) {
            uint32_t incoming_block = inst.arg(i * 2);
            uint32_t incoming_value = inst.arg(i * 2 + 1);
            if (incoming_block == prev_block_off) {
                phi_results.push_back({inst_off, slots[incoming_value]});
                break;
            }
        }
        pos += 4;
    }

    // Phase 2: 批量写入（parallel assignment）
    for (auto& [dst, val] : phi_results) {
        slots[dst] = val;
    }

    return pos;  // 第一条非 PHI 指令的 pos
}
```

**不变量**：
- PHI 只出现在 block 开头，且连续排列
- `resolvePhis` 消费所有 PHI 后返回的 pos 指向第一条非 PHI 指令
- 主循环 (§3.2 Step 2) 从该 pos 开始，assert 不会遇到 PHI
- `executeInst` 的 switch 中不需要 PHI 的 case——如果触达则 assert fail

---

## 4. Instruction Execution

每条指令的解释逻辑是一个 switch case。以下按类别列出。

### 4.1 Constants and Arguments

```
const:   slots[inst_off].bits = inst.constPayload()
funarg:  // 已在 initFunArgs 中设置，执行时无操作
```

### 4.2 Arithmetic

```
add:  slots[R] = slots[A].asI64() + slots[B].asI64()
sub:  slots[R] = slots[A].asI64() - slots[B].asI64()
mul:  slots[R] = slots[A].asI64() * slots[B].asI64()
div:  slots[R] = slots[A].asI64() / slots[B].asI64()    // B==0 → trap 或返回 0
udiv: slots[R] = slots[A].asU64() / slots[B].asU64()
mod:  slots[R] = slots[A].asI64() % slots[B].asI64()
neg:  slots[R] = -slots[A].asI64()
```

其中 R = inst_off（结果的 slot），A = inst.arg(0)，B = inst.arg(1)。

根据 `inst.type` 区分 32-bit 和 64-bit 运算：

```cpp
case Opcode::Add:
    if (inst.type == TypeId::Int32)
        slots[inst_off].bits = (uint64_t)(int64_t)(
            slots[inst.arg(0)].asInt32() + slots[inst.arg(1)].asInt32());
    else if (inst.type == TypeId::Int64)
        slots[inst_off].bits = (uint64_t)(
            slots[inst.arg(0)].asInt64() + slots[inst.arg(1)].asInt64());
    // ...
```

### 4.3 Checked Arithmetic

```
checkedadd:
    检测溢出（使用 __builtin_add_overflow 或手动检查）
    if overflow:
        return BRANCH to inst.arg(3)   // overflow block
    else:
        slots[R] = result
        return BRANCH to inst.arg(2)   // continue block
```

### 4.4 Bitwise

```
and:   slots[R] = slots[A].bits & slots[B].bits
or:    slots[R] = slots[A].bits | slots[B].bits
xor:   slots[R] = slots[A].bits ^ slots[B].bits
shl:   slots[R] = slots[A].bits << slots[B].asI64()
ashr:  slots[R] = (int64_t)slots[A].bits >> slots[B].asI64()
lshr:  slots[R] = slots[A].bits >> slots[B].asI64()
rotl:  slots[R] = rotateLeft(slots[A].bits, slots[B].asI64())
rotr:  slots[R] = rotateRight(slots[A].bits, slots[B].asI64())
```

`rotateLeft`/`rotateRight` 使用 C++20 `std::rotl`/`std::rotr` 或手动实现：

```cpp
uint64_t rotateLeft(uint64_t v, int n) {
    n &= 63;
    return (v << n) | (v >> (64 - n));
}
```

### 4.5 Comparison

```
cmpeq: slots[R] = (slots[A].asI64() == slots[B].asI64()) ? 1 : 0
cmpne: slots[R] = (slots[A].asI64() != slots[B].asI64()) ? 1 : 0
cmplt: slots[R] = (slots[A].asI64() <  slots[B].asI64()) ? 1 : 0
cmple: slots[R] = (slots[A].asI64() <= slots[B].asI64()) ? 1 : 0
cmpgt: slots[R] = (slots[A].asI64() >  slots[B].asI64()) ? 1 : 0
cmpge: slots[R] = (slots[A].asI64() >= slots[B].asI64()) ? 1 : 0
```

结果始终为 0 或 1（Bool）。

### 4.6 Logical

```
lnot:   slots[R] = slots[A].asBool() ? 0 : 1
land:   slots[R] = (slots[A].asBool() && slots[B].asBool()) ? 1 : 0
lor:    slots[R] = (slots[A].asBool() || slots[B].asBool()) ? 1 : 0
select: slots[R] = slots[A].asBool() ? slots[B] : slots[C]
```

### 4.7 Cast

```
sext:     slots[R] = signExtend(slots[A], srcBits, dstBits)
zext:     slots[R] = zeroExtend(slots[A], srcBits, dstBits)
trunc:    slots[R] = slots[A].bits & mask(dstBits)
bitcast:  slots[R] = slots[A]          // 原样复制 bits
ptr2int:  slots[R] = slots[A]          // 原样复制（ptr 就是 uint64）
int2ptr:  slots[R] = slots[A]          // 原样复制
```

sign-extend 的实现：

```cpp
int64_t signExtend(uint64_t val, int fromBits, int toBits) {
    int shift = 64 - fromBits;
    return ((int64_t)(val << shift)) >> shift;  // 算术右移传播符号位
}
```

### 4.8 Memory

```
load:
    void* addr = slots[A].asPtr()
    switch (inst.type):
        Int8:  slots[R] = *(int8_t*)addr
        Int32: slots[R] = *(int32_t*)addr
        Int64: slots[R] = *(int64_t*)addr
        Ptr:   slots[R] = (uint64_t)*(void**)addr

store:
    void* addr = slots[B].asPtr()      // arg(1) = 地址
    uint64_t val = slots[A].bits       // arg(0) = 值
    switch (type of A):
        Int8:  *(int8_t*)addr  = (int8_t)val
        Int32: *(int32_t*)addr = (int32_t)val
        Int64: *(int64_t*)addr = (int64_t)val
        Ptr:   *(void**)addr   = (void*)val

getelementptr:
    void* base = slots[A].asPtr()
    int64_t index = slots[B].asI64()
    int elemSize = sizeOf(inst.type)               // GEP 的 type 参数
    slots[R] = (uint64_t)((char*)base + index * elemSize)
```

### 4.9 Control Flow

```
br:
    return ExecuteResult::BRANCH(inst.arg(0))      // target block byte offset

condbr:
    bool cond = slots[inst.arg(0)].asBool()
    if cond:
        return ExecuteResult::BRANCH(inst.arg(1))  // true block
    else:
        return ExecuteResult::BRANCH(inst.arg(2))  // false block

ret:
    return ExecuteResult::RETURN(slots[inst.arg(0)].bits)

retvoid:
    return ExecuteResult::RETURN(0)

call:
    // 从 function table 获取 callee 信息
    uint32_t callee_idx = inst.arg(0)
    // 收集参数
    vector<uint64_t> call_args
    for i in 1..numArgs:
        call_args.push_back(slots[inst.arg(i)].bits)
    // 区分 host function 和 IR function
    if isHostFunction(callee_idx):
        slots[R] = callHostFunction(callee_idx, call_args)
    else:
        slots[R] = execute(program, callee_idx, call_args)  // 递归解释
```

### 4.10 DB-Specific

```
isnull:
    slots[R] = slots[A].asBool() ? 1 : 0       // null_flag 就是 Bool

crc32:
    uint64_t data = slots[A].asU64()
    uint64_t seed = slots[B].asU64()
    slots[R] = _mm_crc32_u64(seed, data)        // 使用 SSE4.2 intrinsic
    // 或软件实现作为 fallback

bswap:
    slots[R] = __builtin_bswap64(slots[A].bits)  // 或按 type 选择 bswap32
```

CRC32 的软件 fallback：

```cpp
uint64_t softCrc32(uint64_t seed, uint64_t data) {
    // 使用查表法实现 CRC32C（与 SSE4.2 的 crc32 指令兼容）
    uint32_t crc = (uint32_t)seed;
    for (int i = 0; i < 8; i++) {
        crc = crc32c_table[(crc ^ (data & 0xFF)) & 0xFF] ^ (crc >> 8);
        data >>= 8;
    }
    return crc;
}
```

---

## 5. Host Function Calls

Interpreter 调用预编译的 C++ 函数（如 hash table 操作）需要处理 calling convention。

### 5.1 Host Function Registry

```cpp
struct HostFunction {
    void* func_ptr;        // C++ 函数指针
    TypeId return_type;
    vector<TypeId> param_types;
};

// 在 codegen 阶段注册，interpreter 执行时查找
vector<HostFunction> host_functions;
```

### 5.2 调用分派

由于 host function 的签名在编译期未知（参数数量、类型各异），使用 `libffi` 或手动按参数数量分派：

```cpp
uint64_t callHostFunction(uint32_t idx, span<uint64_t> args) {
    auto& hf = host_functions[idx];
    switch (args.size()) {
        case 0: return ((uint64_t(*)())hf.func_ptr)();
        case 1: return ((uint64_t(*)(uint64_t))hf.func_ptr)(args[0]);
        case 2: return ((uint64_t(*)(uint64_t,uint64_t))hf.func_ptr)(args[0], args[1]);
        case 3: return ((uint64_t(*)(uint64_t,uint64_t,uint64_t))hf.func_ptr)
                       (args[0], args[1], args[2]);
        // ... 最多支持到 6 个参数（System V ABI 寄存器参数数量）
    }
}
```

> 这里利用了所有参数和返回值都是 64-bit 的事实（与 value slot 一致）。Host function 的 C++ 签名必须使用 `uint64_t` 或等宽类型。实际值的解释（int32/int64/ptr）由 host function 内部处理。

---

## 6. 与 Flying Start 的对比

| 方面 | Interpreter | Flying Start |
|------|-------------|--------------|
| 编译时间 | 0 | ~0.15 ms (Q1) |
| 执行速度 | 慢 ~10-50x | 接近原生 |
| 实现复杂度 | 低（~500 行）| 高（~2000 行）|
| 调试便利性 | 可单步、可打印每条指令的值 | 需要 GDB JIT interface |
| IR 读取方式 | 共享 block discovery + instruction decode | 共享 |
| 用途 | 测试 oracle、调试 | 生产执行 |

### 6.1 共享代码

Interpreter 和 Flying Start 共享以下组件（避免重复实现）：

- **Block discovery**: `discoverBlocks(entry_block_offset)` → 可达 block offset 集合
- **Instruction decode**: `decodeInst(inst_buffer, offset)` → `InstructionView`
- **parseBlock**: 从 block_buffer 解析 `BlockView`（见 doc/flying-start.md §2.2）
- **Host function registry**: 同一套注册接口

---

## 7. Debug Features

### 7.1 Trace Mode

可选的指令级 trace，输出每条指令的执行和值：

```
[entry @0] %0 = funarg ptr → 0x7fff1000
[entry @0] %3 = const i32 5 → 5
[entry @0] %10 = cmpgt i32 %0, %3 → 1
[entry @0] condbr %10 → yes @16
[yes @16] %24 = const i32 1 → 1
[yes @16] ret i32 %24 → 1
```

通过环境变量或 API flag 开启：

```cpp
Interpreter interp;
interp.setTraceEnabled(true);
interp.execute(program, func, args);
```

### 7.2 Breakpoints

> **[MVP deferred]** Breakpoints 在 MVP 中不实现。Trace mode 已足够用于调试。如需要，未来可添加 `addBreakpoint(inst_offset)` 和 `setBreakpointHandler` 接口。

---

## 8. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| 论文有此 backend | **没有**——这是 MVP 新增的 | 见 §1 |
| 高性能解释（threaded code / computed goto）| 简单 switch-case | 性能不重要，正确性优先 |
| libffi 通用调用 | Host function 使用 `std::function<uint64_t(const uint64_t*, uint8_t)>` 统一签名 | Q1 的 host function 参数数 ≤ 3 |
| 值存储优化（只为有值指令分配 slot）| 按 instruction buffer 大小分配 `uint64_t` 数组 | 浪费几 KB，换 O(1) 查找 |
| `switch` 指令 | 不实现 | Q1 不需要 |
| Breakpoints (§7.2) | 不实现——trace mode 已足够 | 可后续添加 |
