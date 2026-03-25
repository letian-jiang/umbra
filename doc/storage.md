# Storage Design Document

## 1. Overview

Storage 层管理内存中的表数据和元数据。MVP 采用最简单的纯列存格式——每列是一个连续的定长数组，无压缩、无分块、无索引。

```
┌──────────────────────────────────────────────────────┐
│                      Catalog                         │
│  tables: { "lineitem" → TableDef }                   │
└───────────────────────┬──────────────────────────────┘
                        │
                        ▼
┌──────────────────────────────────────────────────────┐
│                     Table                            │
│  name: "lineitem"                                    │
│  row_count: 60175                                    │
│  columns: [Column0, Column1, ..., Column15]          │
└───────────────────────┬──────────────────────────────┘
                        │
            ┌───────────┼───────────┐
            ▼           ▼           ▼
      ┌──────────┐ ┌──────────┐ ┌──────────┐
      │ Column 0 │ │ Column 1 │ │   ...    │
      │ int32[]  │ │ int32[]  │ │          │
      └──────────┘ └──────────┘ └──────────┘
      contiguous   contiguous
      array        array
```

### 设计原则

1. **列存**：每列独立存储，scan 时只访问需要的列（配合 projection pushdown）
2. **定长优先**：所有类型映射到定长表示，VARCHAR 用 inline ptr+len 结构
3. **零拷贝访问**：JIT 生成的代码直接通过指针算术访问列数据，无抽象层开销
4. **生命周期简单**：表数据在进程启动时加载，进程结束时释放，无动态增删

---

## 2. Type System → Physical Representation

| SQL Type | C++ Type | Size | Storage 表示 | Notes |
|----------|----------|------|-------------|-------|
| INTEGER | `int32_t` | 4B | 原样存储 | |
| BIGINT | `int64_t` | 8B | 原样存储 | |
| DECIMAL(p,s) | `int64_t` | 8B | Scaled integer: value × 10^s | s 记录在 ColumnDef.type 中 |
| DATE | `int32_t` | 4B | 距 epoch 的天数 | epoch = 2000-01-01 |
| VARCHAR(n) | `StringValue` | 16B | `{const char* ptr, uint32_t len, uint32_t _pad}` | 指向 string buffer |
| BOOLEAN | `int8_t` | 1B | 0 或 1 | 仅内部使用 |

### 2.1 DECIMAL

DECIMAL(15,2) 的值 `123.45` 存储为 `int64_t` 的 `12345`。scale = 2 记录在类型元信息中，运算时由 SQL Values 层处理定标。

`int64_t` 可精确表示 DECIMAL(18,s) 范围内的所有值（|value| < 10^18 < 2^63），满足 TPC-H 所有 DECIMAL 列的精度需求。

### 2.2 DATE

天数偏移量，epoch 为 2000-01-01（PostgreSQL 惯例）：
- `0` = 2000-01-01
- `-730` = 1998-01-01（大致值）
- 负数表示 epoch 之前的日期

Q1 的 `DATE '1998-09-02'` 在 binder 阶段被 constant fold 为对应的 `int32_t` 值。比较时直接整数比较。

### 2.3 VARCHAR

```cpp
struct StringValue {
    const char* ptr;    // 指向 string buffer 中的字符数据（不以 \0 结尾）
    uint32_t len;       // 字节长度
    uint32_t _pad;      // 填充到 16 字节对齐
};
```

**定长 16 字节**——每行的 StringValue 占固定大小，指针指向外部的 string buffer。这使得列数组仍然是定长的，可以用 `base + row_idx * 16` 直接寻址。

字符数据本身存储在 Table 级别的 string buffer 中，加载时一次性分配。

---

## 3. Column

```cpp
struct Column {
    void* data;           // 指向列数据数组的起始地址
    uint32_t elem_size;   // 每个元素的字节大小
    // data[i] 位于 (char*)data + i * elem_size
};
```

列数据是一段 `malloc`/`new` 分配的连续内存。元素按行号密集排列，无 gap、无 null bitmap。

> **[MVP simplification]** Q1 的 lineitem 表没有 NULL 值。MVP 不在 Column 级别存储 null bitmap。SQL Values 层的 null_flag 始终为 `const false`。如果未来支持含 NULL 的表，需要在 Column 中增加 `uint8_t* null_bitmap`（1 bit per row）。

### 3.1 内存布局示例

lineitem 表，row_count = 60175, column 4 (l_quantity, DECIMAL(15,2) = int64_t):

```
Column.data → ┌─────────────────────────────────┐
              │ row 0: int64_t (e.g., 1700)      │  offset = 0
              │ row 1: int64_t (e.g., 3600)      │  offset = 8
              │ row 2: int64_t (e.g., 800)       │  offset = 16
              │ ...                               │
              │ row 60174: int64_t (...)          │  offset = 481392
              └─────────────────────────────────┘
              total = 60175 × 8 = 481,400 bytes
```

---

## 4. Table

```cpp
struct Table {
    std::string name;
    uint64_t row_count;
    std::vector<Column> columns;        // 按 column_index 排列

    // string storage
    std::vector<char> string_buffer;    // 所有 VARCHAR 列的字符数据
};
```

### 4.1 Q1 lineitem 内存占用估算 (SF=0.01, ~60K rows)

| Column | Type | Elem Size | Total |
|--------|------|-----------|-------|
| l_orderkey (0) | int32 | 4B | 240 KB |
| l_partkey (1) | int32 | 4B | 240 KB |
| l_suppkey (2) | int32 | 4B | 240 KB |
| l_linenumber (3) | int32 | 4B | 240 KB |
| l_quantity (4) | int64 | 8B | 480 KB |
| l_extendedprice (5) | int64 | 8B | 480 KB |
| l_discount (6) | int64 | 8B | 480 KB |
| l_tax (7) | int64 | 8B | 480 KB |
| l_returnflag (8) | StringValue | 16B | 960 KB |
| l_linestatus (9) | StringValue | 16B | 960 KB |
| l_shipdate (10) | int32 | 4B | 240 KB |
| l_commitdate (11) | int32 | 4B | 240 KB |
| l_receiptdate (12) | int32 | 4B | 240 KB |
| l_shipinstruct (13) | StringValue | 16B | 960 KB |
| l_shipmode (14) | StringValue | 16B | 960 KB |
| l_comment (15) | StringValue | 16B | 960 KB |
| **列数据合计** | | | **~7.6 MB** |
| string_buffer | | | **~2.5 MB** |
| **总计** | | | **~10 MB** |

Projection pushdown 后 Q1 只访问 7 列（4,5,6,7,8,9,10），实际触及的内存约 **~3.6 MB**。

---

## 5. Catalog

```cpp
struct ColumnDef {
    std::string name;           // "l_shipdate"
    SQLType type;               // DATE, DECIMAL(15,2), VARCHAR(10), ...
    uint32_t column_index;      // 在 Table.columns 中的位置
};

struct TableDef {
    std::string name;           // "lineitem"
    std::vector<ColumnDef> columns;
};

struct Catalog {
    std::unordered_map<std::string, TableDef> tables;

    TableDef* lookupTable(const std::string& name);
    ColumnDef* lookupColumn(const TableDef& table, const std::string& name);
};
```

Catalog 在数据加载时构建，之后只读。Binder 通过 Catalog 将 SQL 中的表名和列名解析为 `(table_index, column_index)` 对。

---

## 6. Data Loading

### 6.1 TPC-H dbgen

使用 `dbgen` 工具生成 TPC-H 数据。MVP 支持两种方式：

1. **外部生成**：调用 `dbgen -s 0.01`，生成 `.tbl` 文件，启动时读取
2. **内嵌生成**：链接 dbgen 的 C 代码，直接在内存中生成（避免磁盘 I/O）

MVP 先用方式 1（简单），后续可切换到方式 2。

### 6.2 .tbl 文件格式

dbgen 输出 `|` 分隔的文本文件：

```
1|155190|7706|1|17|21168.23|0.04|0.02|N|O|1996-03-13|1996-02-12|1996-03-22|DELIVER IN PERSON|TRUCK|egular courts above the|
1|67310|7311|2|36|45983.16|0.09|0.06|N|O|1996-04-12|1996-02-28|1996-04-20|TAKE BACK RETURN|MAIL|ly final dependencies: slyly bold |
```

### 6.3 Loading 流程

```
loadTable("lineitem.tbl"):
    1. 读取文件，逐行解析
    2. 对每行：
       a. split by '|'
       b. 对每个字段，根据 ColumnDef.type 解析：
          - INTEGER:  atoi → int32_t
          - DECIMAL:  parseDecimal("21168.23", scale=2) → int64_t(2116823)
          - DATE:     parseDate("1996-03-13") → daysSinceEpoch(1996,3,13) → int32_t
          - VARCHAR:  将字符串追加到 string_buffer，记录 (ptr, len)
       c. 将解析后的值追加到对应 Column.data
    3. 记录 row_count
    4. 构建 Catalog entry
```

### 6.4 DECIMAL 解析

```
parseDecimal("21168.23", scale=2):
    // 找到小数点位置
    // 整数部分 = 21168, 小数部分 = 23
    // 结果 = 21168 * 10^2 + 23 = 2116823
    // 如果小数位数 < scale，补零
    // 如果小数位数 > scale，截断（或报错）
```

### 6.5 DATE 解析

```
parseDate("1996-03-13"):
    year=1996, month=3, day=13
    // 计算距 epoch (2000-01-01) 的天数
    // 使用标准公历算法
    return daysSinceEpoch(year, month, day)  // 负数，因为在 epoch 之前
```

---

## 7. JIT Code 如何访问 Storage

### 7.1 RuntimeContext

JIT 编译的 pipeline 函数通过 `RuntimeContext` 指针访问表数据。这个指针作为函数的第一个参数传入（RDI 寄存器）。

```cpp
struct RuntimeContext {
    // Table scan 需要的信息
    void** column_ptrs;      // 数组：column_ptrs[col_idx] = Column.data
    uint64_t row_count;

    // Hash aggregation
    void* hash_table;        // HashTable 对象指针

    // Result output
    void* result_buffer;
    uint64_t result_row_count;
};
```

### 7.2 Codegen 访问模式

Tidy Tuples 的 TableScanOperator 生成的 IR 代码访问列数据的模式（见 doc/tidy-tuples.md §6.2）：

```
// 从 RuntimeContext 加载列指针
%ctx = funarg ptr 0                            // RuntimeContext*
%col_ptrs = getelementptr ptr %ctx, i32 0      // &ctx->column_ptrs
%col_ptrs_val = load ptr %col_ptrs             // ctx->column_ptrs (void**)

// 加载第 col_idx 列的基地址
%col_ptr_ptr = getelementptr ptr %col_ptrs_val, i32 col_idx   // &column_ptrs[col_idx]
%col_base = load ptr %col_ptr_ptr              // column_ptrs[col_idx] = Column.data

// 在 scan loop 中，加载第 i 行的值
%elem_ptr = getelementptr <elem_type> %col_base, i64 %i   // col_base + i * sizeof(elem)
%value = load <elem_type> %elem_ptr
```

Flying Start 将 `getelementptr` 链折叠为 x86 复合寻址模式（见 doc/flying-start.md §5.3）：

```x86
; 假设 col_base 在 r12, row_index 在 r9, elem_size=8 (int64)
mov  rax, [r12 + r9*8]     ; 一条指令完成地址计算 + 加载
```

### 7.3 列指针的传递路径

```
启动时:
    Table table = loadTable("lineitem.tbl")

编译时:
    CompilationContext.catalog → 获取 ColumnDef (类型、index)
    codegen emit: column_ptrs[col.index] → 得到列基地址

执行时:
    RuntimeContext ctx;
    ctx.column_ptrs = table.getColumnPointers()  // void*[]
    ctx.row_count = table.row_count
    pipeline_func(&ctx)                          // JIT 函数直接访问
```

---

## 8. VARCHAR 访问细节

VARCHAR 列在 Column.data 中存储的是 `StringValue` 数组（16B per element）。字符数据在 `Table.string_buffer` 中。

```
Column.data (l_returnflag):
  row 0: StringValue { ptr=&buf[0],    len=1 }   → "N"
  row 1: StringValue { ptr=&buf[1],    len=1 }   → "N"
  row 2: StringValue { ptr=&buf[2],    len=1 }   → "A"
  ...

Table.string_buffer:
  [N][N][A][F][N][R][A]...
```

对 Q1 的 group-by keys（`l_returnflag`, `l_linestatus`，都是 VARCHAR(1)），hash 和比较只需访问 `StringValue.ptr` 指向的 1 个字节。

codegen 中访问 VARCHAR 的模式：

```
// 加载 StringValue (16 bytes)
%sv_ptr = getelementptr StringValue %col_base, i64 %i
%str_ptr = load ptr %sv_ptr                      // StringValue.ptr
%str_len = load i32 (sv_ptr + 8)                 // StringValue.len

// 对 VARCHAR(1)，直接加载第一个字节用于 hash/compare
%first_byte = load i8 %str_ptr
```

> 对 Q1 来说，`l_returnflag` 和 `l_linestatus` 都是 VARCHAR(1)，可以优化为直接按 `int8` hash 和比较，跳过 StringValue 的间接访问。这是一个 codegen 层面的优化，storage 层不需要特殊处理。

---

## 9. MVP Simplifications Summary

| 完整系统特性 | MVP 处理 | 影响 |
|------------|---------|------|
| 磁盘存储 / buffer pool | 纯内存，启动时全量加载 | SF=0.01 ~10MB，无需磁盘 |
| 压缩（dictionary, RLE, ...）| 无压缩 | 内存占用略大，但避免解压开销 |
| Chunking / morsel | 单个连续数组 per column | 无法并行 scan，MVP 单线程 |
| NULL bitmap | 不存储 | Q1 的 lineitem 无 NULL |
| 增删改（INSERT/UPDATE/DELETE）| 只读，启动时一次性加载 | |
| MVCC / 事务 | 无 | |
| 多表 | 只加载 lineitem | Q1 单表 |
| 统计信息（histogram, min/max）| 不收集 | optimizer 不需要 |
| String 优化（short string inline）| 统一 ptr+len 结构 | 简单但 VARCHAR(1) 有间接访问开销 |
