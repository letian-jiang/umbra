# Inline Host Functions 设计文档

## 1. 动机

### 1.1 论文参考（Tidy Tuples §2.6 Host Language Integration）

论文的核心观点：代码生成框架需要同时支持「生成代码」和「调用预编译 C++ 代码」两种模式。Codegen 的 proxy 系统允许程序员为 C++ 函数标记 **inline marker**：

- **Flying Start**：忽略 inline marker，直接调用预编译函数（快速编译）
- **LLVM 优化后端**：将函数体内联到所有调用点（消除调用开销）

> "The proxy system allows that a programmer can mark functions to be inlined. The Flying Start backend will ignore this inlining marker and only profit from lower compilation time. Our optimizing backend, which aims for peak performance, will react to the marker and inline the function at all call sites, thus removing any calling overhead."

这套机制实现了：**用 C++ 编写功能、在生成代码中调用、减少代码生成时间、但无运行时开销**。

### 1.2 当前实现的问题

现有 host function 调用链路有三层开销：

```
生成代码 → 参数打包 (alloca + store) → umbraCallHost trampoline → std::function 间接调用 → 实际函数
                  ↑                          ↑                              ↑
              参数序列化开销              额外函数调用            虚函数表 / type erasure
```

具体问题：

1. **参数序列化**：每个参数先转为 `i64`，存入 `alloca [N x i64]` 数组，被调方再逆向提取。纯粹浪费。
2. **Trampoline 间接调用**：`umbraCallHost(void* hostFunc, const uint64_t* args, uint8_t count)` 在运行时解引用 `std::function`，增加一次完整的 call/ret 开销。
3. **优化屏障**：LLVM 看不透 `umbraCallHost` 的间接调用，无法做任何跨函数优化（常量传播、循环外提、内联等）。

### 1.3 Q1 影响分析

TPC-H Q1 SF=0.01 的 build pipeline 内循环：

```
for each row (≈60K):
    hash(keys)
    ht_lookup(table, hash)      ← hot host call
    if null: ht_insert(...)     ← cold (仅 4 次)
    else: compare keys
    update aggregates
```

`ht_lookup` 在内循环中被调用约 60K 次。当前每次调用开销约 15-20ns（参数打包 + trampoline + std::function），占总执行时间的不可忽略比例。

---

## 2. 方案概述

分两级递进实现，每级独立可用：

| 级别 | 方案 | 效果 | 复杂度 |
|------|------|------|--------|
| Level 1 | Direct C ABI Call | 消除 trampoline + 参数序列化 | 低 |
| Level 2 | Embedded Bitcode Inlining | 完全内联、跨函数优化 | 中 |

### 各后端行为对比

| 调用方式 | Interpreter | Flying Start | LLVM (Level 1) | LLVM (Level 2) |
|----------|-------------|-------------|-----------------|-----------------|
| 参数传递 | `uint64_t[]` | `uint64_t[]` | 原生 C ABI 寄存器传参 | N/A（已内联） |
| 调用机制 | dispatch loop | 跳板函数 | `call` 直接调用 | 函数体内联 |
| LLVM 可见 | N/A | N/A | 声明可见 | 定义可见 |
| 调用开销 | 高 | 中 | 低（~3ns） | 零 |

---

## 3. Level 1: Direct C ABI Call

### 3.1 原理

现有方案：所有 host function 统一走 `umbraCallHost(void*, const uint64_t*, uint8_t)` 跳板。

改进方案：为每个 host function 声明其**真实 C ABI 签名**，LLVM 直接调用，不经过跳板。

```
// 现有                                    // 改进后
%hostPtr = inttoptr i64 <addr> to i8*       declare i8* @umbra_ht_lookup(i8*, i64)
%args = alloca [2 x i64]
store i64 %table, %args[0]                  %result = call i8* @umbra_ht_lookup(
store i64 %hash, %args[1]                       i8* %table, i64 %hash)
%result = call i64 @umbraCallHost(
    i8* %hostPtr, i64* %args, i8 2)
%ptr = inttoptr i64 %result to i8*
```

### 3.2 `extern "C"` 包装函数

为 hot path 上的 host function 提供 `extern "C"` 版本（稳定 C ABI，无 name mangling）：

```cpp
// src/backend/llvm/host_functions_c.h

extern "C" {

/// Hash table lookup: returns payload pointer or nullptr.
uint8_t* umbra_ht_lookup(void* table, uint64_t hash);

/// Hash table insert: returns payload pointer of new entry.
uint8_t* umbra_ht_insert(void* table, uint64_t hash);

/// Hash table probe next: linear probe to next matching slot.
uint8_t* umbra_ht_probe_next(void* table, uint8_t* current_entry, uint64_t hash);

/// Hash table accessors (for scan loop).
uint64_t umbra_ht_get_capacity(void* table);
uint8_t* umbra_ht_get_slot_buffer(void* table);
uint64_t umbra_ht_get_slot_stride(void* table);

}  // extern "C"
```

实现（thin wrapper，直接委托给 HashTable 成员函数）：

```cpp
// src/backend/llvm/host_functions_c.cpp

extern "C" uint8_t* umbra_ht_lookup(void* table, uint64_t hash) {
    return reinterpret_cast<HashTable*>(table)->lookup(hash);
}

extern "C" uint8_t* umbra_ht_insert(void* table, uint64_t hash) {
    return reinterpret_cast<HashTable*>(table)->insert(hash);
}

// ... 其余类似
```

### 3.3 扩展 IR HostFunction 注册

在 `ir::Program` 和 `CodegenContext` 中扩展 host function 注册，支持**直接函数指针**和**内联标记**：

```cpp
// ir/program.h — 扩展 FunctionMeta
struct FunctionMeta {
    uint32_t numParams;
    TypeId returnType;
    std::string name;
    std::vector<TypeId> paramTypes;

    // === NEW ===
    void* directPtr = nullptr;   // extern "C" 函数指针（Level 1）
    bool inlineable = false;     // 内联标记（Level 2）
};
```

```cpp
// codegen/api/codegen_context.h — 扩展注册 API
ir::FunctionId registerHostFunction(
    const std::string& name,
    ir::HostFunction func,              // 通用回调（Interpreter/Flying Start 使用）
    ir::TypeId returnType,
    const std::vector<ir::TypeId>& paramTypes,
    void* directPtr = nullptr,          // NEW: C ABI 函数指针
    bool inlineable = false             // NEW: 内联标记
);
```

### 3.4 LLVM Backend: Direct Call Lowering

当 `FunctionMeta::directPtr != nullptr` 时，LLVM 后端改用直接调用：

```cpp
// llvm_compiler.cpp — translateCall()

case Op::CALL0 ... Op::CALL6: {
    ir::FunctionId calleeId = inst.arg(0);
    const auto& meta = prog.functionMeta(calleeId);

    if (meta.directPtr != nullptr) {
        // === Level 1: Direct C ABI Call ===
        translateDirectCall(meta, inst, numParams);
    } else {
        // Fallback: 现有 trampoline 路径
        translateTrampolineCall(prog, inst, calleeId, numParams);
    }
}
```

`translateDirectCall` 的逻辑：

```cpp
void translateDirectCall(const FunctionMeta& meta, InstructionView inst, uint8_t numParams) {
    // 1. 构建 LLVM 函数类型（使用真实参数类型）
    std::vector<llvm::Type*> paramTys;
    for (auto ty : meta.paramTypes) {
        paramTys.push_back(toLLVMType(ty));
    }
    llvm::Type* retTy = toLLVMType(meta.returnType);
    auto* funcTy = llvm::FunctionType::get(retTy, paramTys, false);

    // 2. 声明函数（或从缓存中获取）
    std::string symbolName = "umbra_" + meta.name;
    llvm::FunctionCallee callee = mod->getOrInsertFunction(symbolName, funcTy);

    // 3. 准备参数（直接类型传递，无需序列化到数组）
    std::vector<llvm::Value*> args;
    for (uint8_t i = 0; i < numParams; ++i) {
        llvm::Value* argVal = getValue(inst.arg(1 + i));
        llvm::Type* expectedTy = paramTys[i];
        // 类型转换（如 i64 → ptr, i32 → i64 等）
        argVal = coerceArg(argVal, expectedTy);
        args.push_back(argVal);
    }

    // 4. 直接调用
    llvm::Value* result = bld->CreateCall(callee, args);

    // 5. 结果类型转换
    if (meta.returnType != ir::TypeId::VOID) {
        result = coerceResult(result, toLLVMType(meta.returnType));
        setValue(instOff, result);
    }
}
```

注册 absolute symbol（在 LLJIT 初始化时）：

```cpp
// 遍历所有 host function，为有 directPtr 的注册 absolute symbol
for (FunctionId fid = 0; fid < prog.numFunctions(); ++fid) {
    const auto& meta = prog.functionMeta(fid);
    if (meta.directPtr != nullptr) {
        std::string sym = "umbra_" + meta.name;
        symbols[jit->mangleAndIntern(sym)] = JITEvaluatedSymbol(
            pointerToJITTargetAddress(meta.directPtr),
            JITSymbolFlags::Exported | JITSymbolFlags::Callable);
    }
}
```

---

## 4. Level 2: Embedded Bitcode Inlining

### 4.1 原理

Level 1 消除了 trampoline 开销，但函数调用本身仍存在（保存/恢复寄存器、call/ret 指令、参数传递）。Level 2 通过将函数体嵌入 LLVM 模块，让 LLVM O3 的 inliner 将函数完全内联到调用点。

关键：LLVM 的 inliner 需要**函数定义**（不仅仅是声明）才能内联。我们需要在 JIT 模块中提供 inlineable 函数的完整 LLVM IR。

### 4.2 Bitcode 生成（System Compile Time）

将 inlineable 函数的实现编译为 LLVM bitcode，嵌入到二进制中：

**步骤 1：Inlineable 函数源文件**

```cpp
// src/backend/llvm/host_inline.cpp
// 仅包含标记为 inlineable 的函数实现。
// 编译为 LLVM bitcode，嵌入到最终二进制。

#include <cstdint>
#include <cstring>

// HashTable 内存布局常量（必须与 hash_table.h 一致）
static constexpr uint32_t HT_OCCUPIED_OFFSET = 0;
static constexpr uint32_t HT_HASH_OFFSET = 1;
static constexpr uint32_t HT_PAYLOAD_OFFSET = 9;

// HashTable 对象内存布局（必须与 HashTable 类布局一致）
struct HashTableLayout {
    uint8_t* slots;
    uint32_t capacity;
    uint32_t numEntries;
    uint32_t entrySize;
    uint32_t slotSize;
};

extern "C" uint8_t* umbra_ht_lookup(void* table_opaque, uint64_t hash) {
    auto* ht = static_cast<HashTableLayout*>(table_opaque);
    uint32_t mask = ht->capacity - 1;
    uint32_t idx = static_cast<uint32_t>(hash) & mask;
    uint32_t stride = ht->slotSize;
    uint8_t* slots = ht->slots;

    for (uint32_t probe = 0; probe < ht->capacity; ++probe) {
        uint32_t slot = (idx + probe) & mask;
        uint8_t* slotPtr = slots + static_cast<size_t>(slot) * stride;

        // Check occupied
        if (slotPtr[HT_OCCUPIED_OFFSET] == 0) {
            return nullptr;
        }
        // Check hash match
        uint64_t storedHash;
        __builtin_memcpy(&storedHash, slotPtr + HT_HASH_OFFSET, 8);
        if (storedHash == hash) {
            return slotPtr + HT_PAYLOAD_OFFSET;
        }
    }
    return nullptr;
}

// umbra_ht_insert 不标记为 inlineable（包含 resize 逻辑，体积大、调用频率低）
```

**步骤 2：CMake 编译 bitcode + 嵌入**

```cmake
if(LLVM_BACKEND)
    # 编译 host_inline.cpp 为 LLVM bitcode
    set(HOST_INLINE_SRC ${CMAKE_SOURCE_DIR}/src/backend/llvm/host_inline.cpp)
    set(HOST_INLINE_BC  ${CMAKE_BINARY_DIR}/host_inline.bc)
    set(HOST_INLINE_INC ${CMAKE_BINARY_DIR}/host_inline_bc.inc)

    add_custom_command(
        OUTPUT ${HOST_INLINE_BC}
        COMMAND clang -emit-llvm -c -O2 -std=c++20
                -o ${HOST_INLINE_BC} ${HOST_INLINE_SRC}
        DEPENDS ${HOST_INLINE_SRC}
        COMMENT "Compiling host inline functions to LLVM bitcode"
    )

    # 将 bitcode 转为 C 数组
    add_custom_command(
        OUTPUT ${HOST_INLINE_INC}
        COMMAND xxd -i < ${HOST_INLINE_BC} > ${HOST_INLINE_INC}
        DEPENDS ${HOST_INLINE_BC}
        COMMENT "Embedding host inline bitcode as C array"
    )

    # 添加为 llvm_compiler.cpp 的依赖
    set_source_files_properties(
        ${CMAKE_SOURCE_DIR}/src/backend/llvm/llvm_compiler.cpp
        PROPERTIES OBJECT_DEPENDS ${HOST_INLINE_INC}
    )
endif()
```

**步骤 3：嵌入为静态数组**

```cpp
// src/backend/llvm/host_inline_bitcode.h
#pragma once
#include <cstdint>
#include <cstddef>

namespace umbra::llvm_backend {

// Generated at build time by xxd
static const uint8_t kHostInlineBitcode[] = {
    #include "host_inline_bc.inc"
};
static const size_t kHostInlineBitcodeSize = sizeof(kHostInlineBitcode);

}  // namespace umbra::llvm_backend
```

### 4.3 Bitcode 链接（Query Compile Time）

在 LLVM 后端编译流程中，检查是否有 inlineable 函数。如果有，加载嵌入的 bitcode 模块并与生成的模块链接：

```cpp
// llvm_compiler.cpp — compile() 方法

void LLVMCompiler::compile(const ir::Program& prog) {
    // Phase 1: 生成 LLVM IR（同现有逻辑）
    generateLLVMIR(prog);

    // Phase 2 (NEW): 链接 inline bitcode
    if (hasInlineableFunctions(prog)) {
        linkInlineBitcode();
    }

    // Phase 3: O3 优化（同现有逻辑——inliner 在此阶段工作）
    runO3Optimization();

    // Phase 4: ORC JIT（同现有逻辑）
    jitCompile();
}
```

`linkInlineBitcode` 的实现：

```cpp
void LLVMCompiler::linkInlineBitcode() {
    // 1. 从嵌入的字节数组解析 bitcode
    auto bufOrErr = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char*>(kHostInlineBitcode),
                        kHostInlineBitcodeSize),
        "host_inline", /*RequiresNullTerminator=*/false);

    auto inlineModOrErr = llvm::parseBitcodeFile(
        bufOrErr->getMemBufferRef(), *impl_->context);
    auto inlineModule = std::move(inlineModOrErr.get());

    // 2. 为 inlineable 函数设置 alwaysinline 属性
    for (auto& fn : *inlineModule) {
        if (!fn.isDeclaration()) {
            fn.addFnAttr(llvm::Attribute::AlwaysInline);
            // InlineHint 也可以，但 AlwaysInline 确保内联
        }
    }

    // 3. 链接到主模块
    //    LinkOnlyNeeded = true: 只链接主模块实际引用的函数
    llvm::Linker::linkModules(*impl_->module, std::move(inlineModule),
                               llvm::Linker::Flags::LinkOnlyNeeded);
}
```

### 4.4 调用点改变

对于 `inlineable = true` 的函数，LLVM 后端在 IR 生成阶段的行为与 Level 1 相同（直接声明 + 直接调用）。区别仅在于 bitcode 链接后，LLVM 模块中有了函数**定义**而非仅**声明**，O3 的 inliner pass 会将其内联。

因此 Level 2 的调用点代码与 Level 1 完全相同。Level 2 的差异仅在编译流程中增加了 bitcode 链接步骤。

### 4.5 LLVM O3 内联后效果

以 `ht_lookup` 在 Q1 内循环中的调用为例，内联前后对比：

**内联前（Level 1: direct call）**：
```llvm
loop.body:
    ; ... hash computation ...
    %entry = call i8* @umbra_ht_lookup(i8* %table, i64 %hash)
    ; call 指令 → 保存调用者寄存器、传参、跳转、返回、恢复寄存器
    %is_null = icmp eq i8* %entry, null
    br i1 %is_null, label %insert, label %check_keys
```

**内联后（Level 2: inlined）**：
```llvm
loop.body:
    ; ... hash computation ...
    ; ht_lookup 体被内联展开:
    %mask = sub i32 %capacity, 1          ; capacity - 1
    %idx = and i32 %hash_lo, %mask        ; hash & mask
    %stride = load i32, i32* %stride_ptr  ; 可被 LICM 提到循环外
    br label %probe_loop

probe_loop:
    %slot = phi i32 [%idx, %loop.body], [%next_slot, %probe_continue]
    %slot_ptr = getelementptr i8, i8* %slots, i64 %byte_off
    %occupied = load i8, i8* %slot_ptr
    %is_empty = icmp eq i8 %occupied, 0
    br i1 %is_empty, label %not_found, label %check_hash

check_hash:
    %stored_hash = load i64, i64* %hash_ptr
    %match = icmp eq i64 %stored_hash, %hash
    br i1 %match, label %found, label %probe_continue

found:
    ; payload = slot_ptr + 9
    br label %check_keys

not_found:
    ; entry = null
    br label %insert
```

LLVM O3 还会进一步优化：
- **LICM (Loop Invariant Code Motion)**：将 `capacity`、`slots`、`stride` 的 load 提到外循环外
- **常量传播**：如果 `entrySize`/`slotSize` 已知，可以简化 GEP 计算
- **向量化**：对 hash 比较的 probe loop，可能触发 LLVM 的 loop vectorizer

---

## 5. 哪些函数应标记为 Inlineable

### 5.1 选择标准

| 标准 | 说明 |
|------|------|
| 调用频率 | 在内循环中被调用（每行一次） |
| 函数体大小 | 足够小（< 50 条 LLVM IR 指令），避免代码膨胀 |
| 优化潜力 | 内联后有跨函数优化机会（LICM、常量传播等） |
| 无副作用 | 纯函数或仅读取数据，不调用 malloc/系统调用 |

### 5.2 Q1 函数分类

| 函数 | 调用频率 | 内联？ | 理由 |
|------|---------|--------|------|
| `ht_lookup` | ~60K/query | **是** | 热路径，函数体小，内联后 LICM 收益大 |
| `ht_probe_next` | ~0-10/query | **是** | 函数体与 lookup 类似，共享内联后可复用优化 |
| `ht_insert` | ~4/query | **否** | 包含 resize 逻辑（malloc + rehash），体积大、频率低 |
| `ht_get_capacity` | 1/query | **否** | 频率极低，内联收益为零 |
| `ht_get_slot_buffer` | 1/query | **否** | 同上 |
| `ht_get_slot_stride` | 1/query | **否** | 同上 |

### 5.3 可扩展性

当新增 operator（如 Join）时，同样可以将其 hot path 上的 host function 标记为 inlineable。例如 hash join 的 probe 函数与 hash aggregate 的 lookup 函数结构相同。

---

## 6. 布局一致性保证

### 6.1 问题

`host_inline.cpp` 中定义的 `HashTableLayout` 结构体必须与 `HashTable` 类的内存布局完全一致。如果 `HashTable` 的字段顺序或类型变更，bitcode 中的布局假设将失效。

### 6.2 解决方案

**方案 A：共享 layout 头文件**

抽取 `HashTable` 的内存布局为一个 `extern "C"` 友好的 POD 结构体：

```cpp
// src/codegen/structures/hash_table_layout.h
// 被 hash_table.h 和 host_inline.cpp 共同 include

struct HashTableFields {
    uint8_t* slots;
    uint32_t capacity;
    uint32_t numEntries;
    uint32_t entrySize;
    uint32_t slotSize;
};

static_assert(offsetof(HashTableFields, slots) == 0);
static_assert(offsetof(HashTableFields, capacity) == 8);
// ...
```

`HashTable` 类以 `HashTableFields` 作为首成员或布局兼容基类。

**方案 B：`static_assert` 编译期检查**

在 `host_functions_c.cpp` 中用 `static_assert` 验证 `offsetof` 匹配：

```cpp
static_assert(offsetof(HashTable, slots_) == offsetof(HashTableLayout, slots));
static_assert(offsetof(HashTable, capacity_) == offsetof(HashTableLayout, capacity));
// ...
```

推荐同时使用 A + B。

---

## 7. 实现计划

### Phase 1: Direct C ABI Call（Level 1）

**涉及文件**：

| 文件 | 变更 |
|------|------|
| `src/ir/function.h` | `FunctionMeta` 增加 `directPtr` 和 `inlineable` 字段 |
| `src/codegen/api/codegen_context.h/.cpp` | `registerHostFunction` 扩展参数 |
| `src/backend/llvm/host_functions_c.h/.cpp` | 新增: `extern "C"` 包装函数 |
| `src/backend/llvm/llvm_compiler.cpp` | 翻译逻辑: trampoline → direct call |
| `src/codegen/structures/hash_table.cpp` | 注册时提供 `directPtr` |
| `test/ir/test_llvm_backend.cpp` | 验证 direct call 路径正确性 |

**步骤**：

1. 在 `FunctionMeta` 中增加 `directPtr` 字段
2. 扩展 `registerHostFunction` API，接受 `directPtr` 参数
3. 创建 `host_functions_c.h/.cpp`，实现 `extern "C"` 包装
4. 修改 `HashTableCodegen::registerHostFunctions`，传入 `directPtr`
5. 修改 LLVM 后端的 CALL lowering：检测 `directPtr`，走直接调用路径
6. 在 LLJIT 中注册 direct function 的 absolute symbol
7. 测试验证

### Phase 2: Embedded Bitcode Inlining（Level 2）

**涉及文件**：

| 文件 | 变更 |
|------|------|
| `src/backend/llvm/host_inline.cpp` | 新增: inlineable 函数实现（编译为 bitcode） |
| `src/codegen/structures/hash_table_layout.h` | 新增: 共享 POD layout |
| `CMakeLists.txt` | bitcode 编译 + xxd 嵌入 |
| `src/backend/llvm/host_inline_bitcode.h` | 新增: 嵌入的 bitcode 数组 |
| `src/backend/llvm/llvm_compiler.cpp` | 增加 bitcode 链接步骤 |
| `src/codegen/structures/hash_table.cpp` | 注册时设置 `inlineable = true` |
| `test/ir/test_llvm_backend.cpp` | 验证内联效果（EXPLAIN ASM） |

**步骤**：

1. 抽取 `HashTableFields` 共享布局 + `static_assert` 检查
2. 创建 `host_inline.cpp`，实现 `umbra_ht_lookup` 和 `umbra_ht_probe_next`
3. CMake 添加 bitcode 编译和嵌入
4. 在 `FunctionMeta` 中增加 `inlineable` 字段
5. 修改 LLVM 后端：检测 inlineable 标记 → 链接 bitcode → 设置 `alwaysinline`
6. 修改 `HashTableCodegen`：为 lookup/probeNext 设置 `inlineable = true`
7. 三后端交叉验证 + EXPLAIN ASM 验证内联效果

---

## 8. 验证方案

### 8.1 正确性

- **三后端交叉验证**：Interpreter、Flying Start、LLVM（with inlining）三者在 TPC-H Q1 上的结果必须完全一致
- **边界测试**：空表 lookup、单 entry 表、hash collision、resize 后的一致性

### 8.2 性能

- **EXPLAIN PROFILE 对比**：对比内联前后的 compile time 和 execute time
- **预期目标**：
  - Level 1: execute time 减少 10-20%（消除 trampoline 开销）
  - Level 2: execute time 进一步减少 10-30%（内联 + LICM + 常量传播）
  - 编译时间增加 < 2ms（bitcode 链接 + 更大的 O3 scope）

### 8.3 EXPLAIN ASM 验证

通过 `EXPLAIN ASM` 输出的 LLVM IR 验证内联是否生效。内联后：
- `@umbra_ht_lookup` 声明应消失
- 函数调用 `call ... @umbra_ht_lookup` 应被替换为展开的 probe 循环
- `capacity`、`slots`、`stride` 的 load 应出现在外循环（LICM 效果）

---

## 9. 与论文 Proxy 系统的对比

| 维度 | 论文 Proxy | 我们的方案 |
|------|-----------|-----------|
| 自动化 | C++ 编译期自动生成代理 | 手动编写 `extern "C"` 包装 |
| 类成员访问 | 生成代码可直接访问类字段 | 通过 POD layout 或 accessor 函数 |
| 内联机制 | 编译期生成 inline 版本 | 嵌入预编译 bitcode，LLVM inliner 内联 |
| 适用范围 | 任意 C++ 类和方法 | 仅标记的 hot function |
| 复杂度 | 高（需要 C++ 元编程） | 低-中（手动标记 + bitcode 流程） |

论文的 proxy 系统是一个通用的 C++ 元编程框架，可以为任意类生成代理。我们的方案针对 MVP 简化：仅为 hot path 上的少数函数提供手动包装。当系统规模增大时，可以考虑引入自动代理生成。
