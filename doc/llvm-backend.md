# LLVM Backend Design Document

## 1. Overview

LLVM Backend 将 Umbra IR lower 到 LLVM IR，利用 LLVM O3 优化管线和 ORC JIT (LLJIT) 编译执行。与 Flying Start 的设计哲学不同：**以更长的编译时间换取更优的执行性能**。

```
Umbra IR Program
     │
     ├── Pass 0: RPO Block Discovery (DFS post-order)
     │
     ├── Pass 1: LLVM IR Generation (two-phase for PHI)
     │      Phase 1: 创建所有 BasicBlock + 空 PHI nodes
     │      Phase 2: RPO 逐指令翻译
     │      Phase 3: 回填 PHI incoming values
     │
     ├── Pass 2: LLVM O3 Optimization (New Pass Manager)
     │
     └── Pass 3: ORC JIT (LLJIT) → native function pointer
              │
              ▼
         Executable Function Pointer
```

### 设计原则

1. **逐指令 lowering**：每条 Umbra IR 指令映射到一组 LLVM IR 指令，保持语义一致
2. **优化交给 LLVM**：不做自定义优化，利用 LLVM O3 的全部能力（常量传播、循环优化、向量化、寄存器分配等）
3. **ABI 兼容**：函数签名与 Flying Start 相同（`i64(i64, ...)` 调用约定），共享 `execute()` trampoline
4. **可选编译**：通过 CMake `LLVM_BACKEND=ON` 开启，不影响无 LLVM 环境的构建

---

## 2. 类型 Lowering

| Umbra `TypeId` | LLVM Type | 说明 |
|---|---|---|
| `VOID` | `void` | 函数级别用 `i64` 返回 0（ABI 兼容） |
| `BOOL` | `i8` | 运行时用 i8 (0/1) 表示，非 i1 |
| `INT8` | `i8` | |
| `INT32` | `i32` | |
| `INT64` | `i64` | |
| `UINT64` | `i64` | LLVM 整数无符号属性，由 cmp 谓词区分 |
| `PTR` | `i8*` | LLVM 14 typed pointer |

### 函数签名

ABI 层面所有参数和返回值统一为 `i64`，与 Flying Start 保持一致：

```llvm
; 算术函数: i64 add(i64, i64)
define i64 @add_fn(i64 %0, i64 %1) {
  ...
}

; Pipeline 函数: void pipeline_build(RuntimeContext*)
; ABI: i64(i64) — RET_VOID 翻译为 ret i64 0
define i64 @pipeline_build(i64 %0) {
  ...
  ret i64 0
}
```

FUNARG 指令在函数入口将 `i64` 参数 trunc/inttoptr 到目标类型。

---

## 3. Lowering Pipeline

### 3.1 Block Discovery

与 Flying Start 相同（见 doc/flying-start.md §2.2），从 `entry_block_offset` 出发 DFS 发现所有可达 block，计算 reverse post-order。

```
discoverBlocks(entryBlockOff):
    // iterative DFS → post-order → reverse = RPO
    stack = {(entryBlockOff, false)}
    visited = {entryBlockOff}
    postOrder = []

    while stack not empty:
        (blockOff, childrenPushed) = stack.top()
        if not childrenPushed:
            mark childrenPushed = true
            terminator = last instruction in block
            for each successor of terminator:
                if not visited: push to stack
        else:
            postOrder.append(blockOff)
            stack.pop()

    rpoOrder = reverse(postOrder)
```

注意 `CHECKED_ADD/SUB/MUL` 是 terminator（有 cont_block 和 overflow_block 两个后继）。

### 3.2 LLVM IR Generation (Three-Phase)

PHI 节点的 incoming value 可能在 forward reference 的 block 中定义。LLVM 的 `PHINode::addIncoming` 需要实际的 `Value*`，因此采用三阶段生成：

**Phase 1 — 创建空 PHI**

遍历每个 block，为开头的 PHI 指令创建空 `llvm::PHINode`，记入 valueMap：

```
for blockOff in rpoOrder:
    builder.setInsertPoint(blockMap[blockOff])
    for inst in block:
        if not isPhi(inst.opcode): break
        phi = builder.CreatePHI(type, numIncoming)
        valueMap[inst.offset] = phi
```

**Phase 2 — 翻译非 PHI 指令**

RPO 遍历每个 block，跳过 PHI，逐指令翻译：

```
for blockOff in rpoOrder:
    builder.setInsertPoint(blockMap[blockOff])
    for inst in block:
        if isPhi(inst.opcode): continue
        translateInstruction(inst)
```

**Phase 3 — 回填 PHI incoming**

所有值都已在 valueMap 中，遍历所有 PHI 节点，添加 incoming：

```
for blockOff in rpoOrder:
    for inst in block:
        if not isPhi(inst.opcode): break
        phi = valueMap[inst.offset]
        for i in 0..numIncoming:
            srcBlock = inst.arg(i*2)
            srcValue = inst.arg(i*2+1)
            phi.addIncoming(valueMap[srcValue], blockMap[srcBlock])
```

### 3.3 常量按需物化

IRBuilder 会对常量进行去重。去重后的 CONST 可能在其被引用的 block 之外定义。在 `getValue(op)` 中，如果 valueMap 未命中且指令为 CONST，则按需创建 `ConstantInt`：

```
getValue(op):
    if op in valueMap: return valueMap[op]
    inst = program.instructionAt(op)
    if inst.opcode == CONST:
        val = ConstantInt(inst.type, inst.constPayload)
        valueMap[op] = val
        return val
    error("value not found")
```

### 3.4 Optimization & JIT

```cpp
// New Pass Manager with O3
PassBuilder pb;
auto mpm = pb.buildPerModuleDefaultPipeline(OptimizationLevel::O3);
mpm.run(*module, MAM);

// ORC JIT
auto jit = LLJITBuilder().create();
// 注册 host function trampoline 为 absolute symbol
jit->getMainJITDylib().define(absoluteSymbols({
    {"umbraCallHost", JITEvaluatedSymbol(&umbraCallHost, ...)}
}));
jit->addIRModule(ThreadSafeModule(module, ctx));
auto sym = jit->lookup(funcName);
```

---

## 4. 逐 Opcode Lowering 规则

### 4.1 Constants & Arguments

| Umbra | LLVM IR |
|---|---|
| `CONST` | `ConstantInt::get(type, payload)` 或 `inttoptr(i64 addr to i8*)` |
| `FUNARG` | `func->getArg(index)` + trunc/inttoptr 到目标类型 |

### 4.2 Arithmetic

| Umbra | LLVM IR | Notes |
|---|---|---|
| `ADD` | `add` | |
| `SUB` | `sub` | |
| `MUL` | `mul` | |
| `DIV` | `sdiv` | 有符号除法 |
| `UDIV` | `udiv` | 无符号除法 |
| `MOD` | `srem` | 有符号取余 |
| `NEG` | `sub 0, val` | Umbra NEG 编码为 binary `(0, operand)` |

### 4.3 Checked Arithmetic

使用 LLVM overflow intrinsics，产出 `{iN, i1}` 结构体：

```llvm
; CHECKED_ADD(lhs, rhs, cont_bb, ovf_bb)
%r = call {i64, i1} @llvm.sadd.with.overflow.i64(i64 %lhs, i64 %rhs)
%val = extractvalue {i64, i1} %r, 0    ; → valueMap[instOff]
%ovf = extractvalue {i64, i1} %r, 1
br i1 %ovf, label %overflow_bb, label %cont_bb
```

| Umbra | LLVM Intrinsic |
|---|---|
| `CHECKED_ADD` | `@llvm.sadd.with.overflow.iN` |
| `CHECKED_SUB` | `@llvm.ssub.with.overflow.iN` |
| `CHECKED_MUL` | `@llvm.smul.with.overflow.iN` |

### 4.4 Bitwise

| Umbra | LLVM IR |
|---|---|
| `AND` | `and` |
| `OR` | `or` |
| `XOR` | `xor` |
| `SHL` | `shl` |
| `ASHR` | `ashr` |
| `LSHR` | `lshr` |
| `ROTL` | `@llvm.fshl.iN(val, val, shamt)` |
| `ROTR` | `@llvm.fshr.iN(val, val, shamt)` |

### 4.5 Comparison

`ICmp` 产出 `i1`，需 `zext` 到 `i8` 以匹配 Umbra BOOL (i8) 语义。

| Umbra | LLVM Predicate | Notes |
|---|---|---|
| `CMP_EQ` | `ICMP_EQ` | |
| `CMP_NE` | `ICMP_NE` | |
| `CMP_LT` | `ICMP_SLT` / `ICMP_ULT` | 操作数为 UINT64 时用无符号 |
| `CMP_LE` | `ICMP_SLE` / `ICMP_ULE` | |
| `CMP_GT` | `ICMP_SGT` / `ICMP_UGT` | |
| `CMP_GE` | `ICMP_SGE` / `ICMP_UGE` | |

符号性判定：检查操作数的 Umbra IR 类型（`instructionAt(arg).type()`），UINT64 → 无符号谓词，其余 → 有符号。

### 4.6 Logical

| Umbra | LLVM IR |
|---|---|
| `LNOT` | `icmp eq i8 %val, 0` → `zext i1 to i8` |
| `LAND` | `and i8` (BOOL 为 0/1，位与 = 逻辑与) |
| `LOR` | `or i8` |
| `SELECT` | `icmp ne i8 %cond, 0` → `select i1, %tv, %fv` |

### 4.7 Cast

| Umbra | LLVM IR |
|---|---|
| `SEXT` | `sext` |
| `ZEXT` | `zext` |
| `TRUNC` | `trunc` |
| `BITCAST` | `bitcast` / `ptrtoint` / `inttoptr` (视源/目标类别) |
| `PTR2INT` | `ptrtoint` |
| `INT2PTR` | `inttoptr` |

### 4.8 Memory

| Umbra | LLVM IR | Notes |
|---|---|---|
| `LOAD` | `bitcast ptr to elemTy*` → `load elemTy, elemTy* ptr` | `inst.type()` 给出元素类型 |
| `STORE` | `bitcast ptr to valTy*` → `store val, valTy* ptr` | |
| `GEP` | `bitcast base to elemTy*` → `gep elemTy, elemTy* base, index` → `bitcast to i8*` | 结果为 PTR (i8*) |

### 4.9 Control Flow

| Umbra | LLVM IR |
|---|---|
| `BR` | `br label %target` |
| `COND_BR` | `icmp ne i8 %cond, 0` → `br i1 %cond1, %trueBB, %falseBB` |
| `RET` | `ret i64 %val` (必要时 sext/trunc 到 i64) |
| `RET_VOID` | `ret i64 0` (ABI 兼容) |

### 4.10 Host Function Calls

通过 C trampoline 调用 `std::function`：

```cpp
// 定义在 llvm_compiler.cpp
extern "C" uint64_t umbraCallHost(void* hostFunc, const uint64_t* args, uint8_t count);
```

Lowering 步骤：

1. 将 `HostFunction*` 地址嵌入为常量 `inttoptr(i64 addr to i8*)`
2. 在函数入口 `alloca [N x i64]` 存放参数（避免循环内 stack 增长）
3. Store 每个参数到 alloca array
4. 调用 trampoline `umbraCallHost(hostPtr, argsArray, count)`

```llvm
; 调用 host function (2 params)
%hostPtr = inttoptr i64 <addr> to i8*
%args = alloca [2 x i64]
store i64 %arg0, [2 x i64]* %args, 0
store i64 %arg1, [2 x i64]* %args, 1
%result = call i64 @umbraCallHost(i8* %hostPtr, i64* %args, i8 2)
```

Trampoline 注册为 LLJIT absolute symbol，LLJIT 在链接时解析。

### 4.11 DB-Specific

| Umbra | LLVM IR |
|---|---|
| `IS_NULL` | `icmp ne %val, 0` → `zext i1 to i8` |
| `CRC32` | `@llvm.x86.sse42.crc32.64.64(i64 %seed, i64 %data)` |
| `BSWAP` | `@llvm.bswap.iN(iN %val)` |

---

## 5. 与 Flying Start 的对比

| 维度 | Flying Start | LLVM Backend |
|------|-------------|-------------|
| 编译策略 | 2-pass 直接翻译 | IR lowering + O3 + JIT |
| 编译时间 | ~0.8ms | ~5-20ms |
| 执行性能 | 良好（手工 4 项优化） | 更优（LLVM 全套优化） |
| 寄存器分配 | 启发式（9 个可分配） | LLVM RA（全部通用寄存器） |
| 指令选择 | 1:1 固定映射 | LLVM 指令选择器 |
| 循环优化 | 无 | LICM、循环展开、向量化 |
| 常量传播 | 仅 IRBuilder 阶段 | 全程序常量传播 |
| 依赖 | asmjit（轻量，FetchContent） | LLVM 14+（系统安装） |
| 适用场景 | 短查询、交互式 | 长时间运行的分析查询 |

### 共享基础设施

- `ir::Program` 数据结构（doc/ir.md）
- `execute()` trampoline（函数指针调用约定相同）
- Block discovery 算法（DFS post-order → RPO）
- `RuntimeContext` 结构和 host function 机制

---

## 6. Build Integration

### CMake

```cmake
# 顶层 CMakeLists.txt
option(LLVM_BACKEND "Enable LLVM JIT backend (requires LLVM 14+)" OFF)
if(LLVM_BACKEND)
    find_package(LLVM 14 REQUIRED CONFIG)
endif()

# src/CMakeLists.txt
if(LLVM_BACKEND)
    target_sources(umbra_lib PRIVATE backend/llvm/llvm_compiler.cpp)
    target_include_directories(umbra_lib SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(umbra_lib PUBLIC UMBRA_LLVM_BACKEND)
    llvm_map_components_to_libnames(llvm_libs core orcjit native support passes)
    target_link_libraries(umbra_lib PRIVATE ${llvm_libs})
endif()
```

- `SYSTEM` include 抑制 LLVM header 在 `-Werror` 下的警告
- `UMBRA_LLVM_BACKEND` 宏守卫所有共享文件中的 LLVM 代码
- 无 LLVM 时 `Backend::LLVM` 枚举值仍存在，运行时抛异常

### 构建命令

```bash
# 带 LLVM 后端
cmake -B build -DCMAKE_BUILD_TYPE=Release -DLLVM_BACKEND=ON
cmake --build build -j$(nproc)

# 不带 LLVM（默认）
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

---

## 7. Shell & EXPLAIN Integration

### .backend 命令

```
umbra> .backend llvm
Backend set to: llvm

umbra> .backend
Current backend: llvm
```

### EXPLAIN ASM

LLVM 后端输出优化后的 LLVM IR（而非 x86 汇编）：

```
umbra> EXPLAIN ASM SELECT ...;
LLVM IR (optimized, O3):
=== Pipeline 0: build ===
; ModuleID = 'umbra_module'
define i64 @pipeline_build(i64 %0) {
  ...
}
```

### EXPLAIN PROFILE

与 Flying Start 相同的 profile 格式，显示 codegen / compile / execute 时间分解：

```
Profile (backend: llvm):
                  plan    codegen  compile  execute  total
Pipeline 0:       -       0.01ms   12.34ms  0.56ms   12.91ms
Pipeline 1:       -       0.01ms   5.67ms   0.01ms   5.69ms
Total:           0.20ms   0.02ms   18.01ms  0.57ms   18.80ms
```

---

## 8. 文件结构

| 文件 | 内容 |
|------|------|
| `src/backend/llvm/llvm_compiler.h` | `LLVMCompiler` 类声明（pimpl 隐藏 LLVM 类型） |
| `src/backend/llvm/llvm_compiler.cpp` | Lowering + O3 优化 + LLJIT 编译执行（全部 LLVM 依赖仅此文件） |
| `test/ir/test_llvm_backend.cpp` | 三后端交叉验证测试 |

`LLVMCompiler` 使用 pimpl 模式将所有 LLVM 类型隐藏在 `Impl` 结构体中，header 不引入任何 LLVM 头文件。

---

## 9. MVP Status

| 特性 | 状态 | 说明 |
|------|------|------|
| 全部 60 opcodes lowering | **已实现** | 所有指令类型完整支持 |
| LLVM O3 优化 | **已实现** | New Pass Manager |
| ORC JIT (LLJIT) | **已实现** | 包含 host function absolute symbol 注册 |
| CMake 可选编译 | **已实现** | `LLVM_BACKEND=ON/OFF` |
| Shell `.backend llvm` | **已实现** | |
| EXPLAIN ASM/PROFILE | **已实现** | |
| 单元测试 (12 cases) | **通过** | 三后端交叉验证 |
| TPC-H Q1 端到端 | **通过** | 三后端交叉验证结果一致 |
| Adaptive execution | 不实现 | MVP 启动时选择 backend |
