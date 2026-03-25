# UmbraDB MVP - Design Document

## 1. Project Goal

Validate that an LLM can develop a JIT-compiling OLAP database similar to Umbra. The MVP targets TPC-H Q1 on SF=0.01, aiming for 70% of Umbra's single-threaded compilation and execution performance.

### TPC-H Q1

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

Q1 is a scan-heavy, aggregation-heavy query. It exercises: table scan with filter, arithmetic expressions (including decimal), hash aggregation with multiple aggregate functions, and ordering. It does NOT require joins, subqueries, or complex optimizer logic — making it ideal for an MVP.

### Performance Target (SF=0.01, single-threaded)

Reference from Tidy Tuples paper Table 2 (SF=0.01, Threads=1):
- Umbra Flying Start: plan=0.19ms, codegen=0.15ms, x86=0.14ms, exec=1.71ms, total=2.19ms
- Our target (70%): total latency < 3.1ms

---

## 2. Architecture Overview

```
Shell (REPL, line editing, history)  ────────────  doc/shell.md
    │
    ▼
SQL string  ──or──  EXPLAIN <level> SQL
    │                       │
    ▼                       ▼
Parser (libpg_query)  ──────────────────────────  doc/parser.md
    │
    ▼
Binder  ─────────────────────────────────────────  doc/binder.md
    │
    ▼
Optimizer  ──────────────────────────────────────  doc/optimizer.md
    │
    ▼
Tidy Tuples (5-layer code generation)  ──────────  doc/tidy-tuples.md
    │
    ▼
Umbra IR  ───────────────────────────────────────  doc/ir.md
    │
    ├──► Interpreter  ───────────────────────────  doc/interpreter.md
    │
    └──► Flying Start (asmjit → x86-64)  ───────  doc/flying-start.md
              │
              ▼
         Executable / Result

EXPLAIN (per-stage inspection)  ─────────────────  doc/explain.md
Storage (in-memory columnar)  ───────────────────  doc/storage.md
Testing  ────────────────────────────────────────  doc/testing.md
```

### Execution Flow

```
1. SQL string
2. Parser: SQL → AST
3. Binder: AST → Bound Logical Plan
4. Optimizer: Logical Plan → Physical Plan → Pipeline[]
5. For each Pipeline:
       Tidy Tuples: Pipeline → Umbra IR Function
       Backend (Interpreter or Flying Start): IR → executable
       Execute: run pipeline function
6. Collect results, format output
```

---

## 3. Design Documents

| Document | Description |
|----------|-------------|
| [doc/storage.md](doc/storage.md) | In-memory columnar storage, type representations, data loading, JIT access patterns |
| [doc/parser.md](doc/parser.md) | libpg_query integration, PG parse tree → internal AST conversion |
| [doc/binder.md](doc/binder.md) | Name resolution, type inference, implicit casts, aggregate analysis, constant folding |
| [doc/optimizer.md](doc/optimizer.md) | Predicate/projection pushdown, physical plan generation, pipeline decomposition |
| [doc/tidy-tuples.md](doc/tidy-tuples.md) | 5-layer code generation framework (Operators → Data Structures → Tuples → SQL Values → Codegen API) |
| [doc/ir.md](doc/ir.md) | Umbra IR: physical memory layout, instruction set (~63 opcodes), builder with backpatch, constant folding + DCE |
| [doc/interpreter.md](doc/interpreter.md) | IR interpreter: value slots, block execution loop, PHI resolution, debug trace |
| [doc/flying-start.md](doc/flying-start.md) | x86-64 JIT backend: live span analysis, register allocation, lazy address calc, cmp-branch fusion |
| [doc/explain.md](doc/explain.md) | EXPLAIN 7 levels: AST, LOGICAL, PHYSICAL, PIPELINE, IR, ASM, PROFILE |
| [doc/shell.md](doc/shell.md) | REPL shell: linenoise line editing, history, tab completion, dot commands, result formatting |
| [doc/testing.md](doc/testing.md) | Unit/IR/SQL/perf tests, Interpreter vs Flying Start cross-validation, DuckDB differential oracle |

---

## 4. Type System

| SQL Type       | Internal Representation | Notes                          |
|----------------|------------------------|--------------------------------|
| INTEGER        | int32_t                |                                |
| BIGINT         | int64_t                |                                |
| DATE           | int32_t                | Days since epoch (2000-01-01)  |
| VARCHAR        | 16-byte inline struct  | ptr + len, no ownership        |
| DECIMAL(p,s)   | int64_t                | Scaled integer, s in metadata  |
| BOOLEAN        | int8_t                 | For internal predicates        |

Every SQL value carries an associated null indicator (1 bit). NULL semantics are handled in the SQL Values layer of Tidy Tuples, following three-valued logic.

---

## 5. Build & Dependencies

| Dependency     | Purpose                              |
|----------------|--------------------------------------|
| C++20          | Language standard                    |
| CMake          | Build system                         |
| libpg_query   | SQL parser                           |
| asmjit         | x86-64 assembler for Flying Start    |
| Catch2         | Test framework                       |
| DuckDB         | Differential testing oracle          |
| dbgen / tpch   | TPC-H data generation                |

---

## 6. File Structure

```
umbra/
├── CMakeLists.txt
├── DESIGN.md
├── doc/                   # design documents
├── src/
│   ├── main.cpp
│   ├── catalog/           # table metadata, schema
│   ├── storage/           # columnar storage, data loading
│   ├── parser/            # libpg_query wrapper, AST
│   ├── binder/            # name resolution, type checking
│   ├── optimizer/         # rule-based optimizer
│   ├── codegen/           # Tidy Tuples framework
│   │   ├── operators/     #   Layer 1: operator translators
│   │   ├── structures/    #   Layer 2: hash table etc.
│   │   ├── tuples/        #   Layer 3: tuple operations
│   │   ├── sql_values/    #   Layer 4: SQL value abstraction
│   │   └── api/           #   Layer 5: IR builder interface
│   ├── ir/                # Umbra IR representation
│   ├── backend/
│   │   ├── interpreter/   # IR interpreter
│   │   └── flying_start/  # x86-64 JIT compiler
│   ├── execution/         # pipeline execution, result materialization
│   └── common/            # types, utilities
├── test/
│   ├── unit/
│   ├── ir/
│   ├── sql/
│   └── perf/
└── third_party/
    ├── libpg_query/
    ├── asmjit/
    ├── catch2/
    └── duckdb/
```

---

## 7. Scope Boundaries

### In Scope (MVP)

- TPC-H Q1 end-to-end
- Single-threaded execution
- In-memory columnar storage
- Interpreter + Flying Start backends
- Hash aggregation with SUM, AVG, COUNT
- Date arithmetic, decimal arithmetic
- Basic EXPLAIN
- Differential testing against DuckDB

### Out of Scope (MVP)

- Joins (hash join, nested loop join)
- Subqueries
- Multiple threads / morsel-driven parallelism
- Disk storage / buffer management
- Cost-based optimizer / statistics
- LLVM backend
- Transactions / MVCC
- String operations beyond simple comparison
- Window functions
- INSERT/UPDATE/DELETE

---

## 8. Reference Papers

- **Tidy Tuples and Flying Start** (Kersten, Leis, Neumann, VLDB Journal 2020): code generation framework, Umbra IR, Flying Start backend → `reference/tidy-tuples.pdf`
- **Quantifying TPC-H Choke Points** (Dreseler et al., PVLDB 2020): TPC-H optimization analysis → `reference/tpch-choke-points.pdf`
