# UmbraDB MVP — Development Plan

## Overview

Serial implementation of a JIT-compiling OLAP database targeting TPC-H Q1 (SF=0.01, <3.1ms).
13 sub-tasks, each independently developed, tested, and reviewed before the next begins.

**Current state**: 11 design docs complete, 0% code implementation.

---

## Dependency Graph

```
Task 1: Build System
   │
   ├──► Task 2: Storage
   │       │
   │       ├──────────────────────────────────► Task 6: Binder ──► Task 7: Optimizer ─┐
   │       │                                        ▲                                  │
   ├──► Task 3: IR                                  │                                  │
   │       │                                   Task 5: Parser                          │
   │       ├──► Task 4: Interpreter                                                    │
   │       │                                                                           │
   │       ├──► Task 8: Codegen API + SQL Values (Layers 5 & 4)                        │
   │       │       │                                                                   │
   │       │       ├──► Task 9: Tuples + Hash Table (Layers 3 & 2)                     │
   │       │       │       │                                                           │
   │       │       │       └──► Task 10: Operator Translators (Layer 1) ◄──────────────┘
   │       │       │                │
   │       │       │                ▼
   │       │       │         Pipeline Execution
   │       │       │
   │       ├──► Task 11: Flying Start JIT
   │       │
   │       └──────────────────────► Task 12: EXPLAIN + Shell
   │
   └──────────────────────────────► Task 13: End-to-End Integration
```

## Execution Order

Tasks are executed serially in the order listed below. Each task produces a commit that passes all checks (build, lint, test, sanitizer, coverage ≥ 90%).

---

## Task 1: Build System + Project Scaffold

**Goal**: Working CMake build with all third-party dependencies, directory structure, and a smoke test.

**Deliverables**:
- `CMakeLists.txt` (top-level + subdirectory CMakeLists)
- Third-party integration: Catch2 (v3, header-only), libpg_query, asmjit
- `.clang-format` and `.clang-tidy` configuration files
- Directory structure per `DESIGN.md` §6
- `src/common/types.h` — basic type aliases and constants
- `src/main.cpp` — minimal placeholder
- `test/unit/test_smoke.cpp` — Catch2 smoke test that compiles and passes
- Verify: `cmake --build build && ctest` passes

**Design docs to update**: None (no design changes)

**Status**: [ ] Not Started

---

## Task 2: Storage Layer

**Goal**: In-memory columnar storage with type system, .tbl data loading, and TPC-H lineitem support.

**Deliverables**:
- `src/common/types.h` — SQLType enum, type sizes, StringRef (ptr+len)
- `src/storage/column.h/.cpp` — Column class with `std::variant<vector<T>>` storage
- `src/storage/table.h/.cpp` — Table class with column array, string buffer, `column_ptrs` cache
- `src/catalog/catalog.h/.cpp` — TableDef, ColumnDef, Catalog registry
- `src/storage/tbl_loader.h/.cpp` — .tbl file parser (pipe-delimited, two-phase load)
- `src/storage/runtime_context.h` — RuntimeContext struct for JIT access
- `test/unit/test_storage.cpp` — Column ops, Table construction, .tbl parsing, DECIMAL/DATE parsing
- TPC-H SF=0.01 data generation setup (dbgen or bundled test data)

**Design docs to update**: `doc/storage.md` if any implementation deviations

**Status**: [ ] Not Started

---

## Task 3: Umbra IR

**Goal**: Complete IR representation with builder, constant folding, DCE, and backpatching.

**Deliverables**:
- `src/ir/opcode.h` — Opcode enum (~63 opcodes), opcode metadata (name, operand count, type)
- `src/ir/instruction.h/.cpp` — Instruction encoding in variable-length bytecode buffer
- `src/ir/block.h` — Block structure (offset into inst_buffer, successor list)
- `src/ir/function.h/.cpp` — Function (entry block, parameter list, block list)
- `src/ir/program.h/.cpp` — Program (vector of Functions, host function registry)
- `src/ir/builder.h/.cpp` — IRBuilder: emit methods for all opcodes, constant dedup, constant folding, DCE
- `src/ir/printer.h/.cpp` — IR text printer (for EXPLAIN IR level and debugging)
- `test/unit/test_ir.cpp` — Builder API, constant folding, DCE, instruction encoding/decoding
- `test/ir/test_ir_programs.cpp` — Hand-written IR programs (arithmetic, control flow, memory, PHI)

**Design docs to update**: `doc/ir.md` if any encoding changes

**Status**: [ ] Not Started

---

## Task 4: Interpreter Backend

**Goal**: Reference IR execution engine, cross-validated with hand-written IR programs.

**Deliverables**:
- `src/backend/interpreter/interpreter.h/.cpp` — Full interpreter:
  - Value slot array (uint64_t per instruction offset)
  - Block execution loop with prev_block tracking
  - PHI resolution (parallel assignment)
  - All opcode implementations (~63)
  - Host function dispatch (up to 6 params)
  - Trace mode for debugging
- `test/ir/test_interpreter.cpp` — Execute hand-written IR programs, verify results:
  - Arithmetic (add, sub, mul, div, mod with all integer types)
  - Comparison and branching
  - Memory load/store
  - Loops with PHI nodes
  - Function calls (host functions)
  - CRC32 instruction
  - NULL propagation patterns

**Design docs to update**: `doc/interpreter.md` if any execution model changes

**Status**: [ ] Not Started

---

## Task 5: Parser

**Goal**: SQL parsing via libpg_query with conversion to internal AST.

**Deliverables**:
- `src/parser/ast.h` — AST node types: SelectStmt, TableRef, ColumnRef, Literal, BinaryExpr, UnaryExpr, FuncCall, CastExpr, DateArith, OrderByItem, AliasedExpr
- `src/parser/ast_context.h/.cpp` — Arena allocator for AST node ownership
- `src/parser/parser.h/.cpp` — libpg_query wrapper: `parse(string) → SelectStmt*`
  - PG protobuf → internal AST conversion
  - Expression building from PG nodes
  - DATE/INTERVAL arithmetic pattern recognition
  - Error handling with source position
- `src/parser/ast_printer.h/.cpp` — AST tree printer (for EXPLAIN AST level)
- `test/unit/test_parser.cpp` — Parse TPC-H Q1, verify AST structure; edge cases (syntax errors, DATE literals, INTERVAL expressions)

**Design docs to update**: `doc/parser.md` if any AST node changes

**Status**: [ ] Not Started

---

## Task 6: Binder

**Goal**: Name resolution, type inference, constant folding, aggregate analysis on parsed AST.

**Deliverables**:
- `src/binder/bound_expr.h` — BoundExpr types: BoundColumnRef, BoundLiteral, BoundBinaryExpr, BoundUnaryExpr, BoundAggCall, BoundCast
- `src/binder/bound_plan.h` — BoundLogicalOperator: BoundScan, BoundFilter, BoundAggregate, BoundSort, BoundProjection
- `src/binder/binder.h/.cpp` — Binder:
  - Scope management: column name → (table_idx, col_idx)
  - Type inference with implicit cast insertion (INT→BIGINT→DECIMAL)
  - Constant folding: DATE arithmetic at compile time
  - Aggregate analysis: AVG → SUM/COUNT decomposition with dedup
  - GROUP BY validation (non-agg exprs must be in GROUP BY)
- `src/binder/bound_plan_printer.h/.cpp` — Logical plan printer
- `test/unit/test_binder.cpp` — Bind Q1, verify types/casts/aggregate decomposition; error cases

**Design docs to update**: `doc/binder.md` if any binding rule changes

**Status**: [ ] Not Started

---

## Task 7: Optimizer

**Goal**: Rule-based optimization producing physical plan with pipeline decomposition.

**Deliverables**:
- `src/optimizer/logical_operator.h` — Logical operators if not already in binder output
- `src/optimizer/physical_operator.h` — PhysicalTableScan, PhysicalHashAggregate, PhysicalSort, PhysicalProjection
- `src/optimizer/optimizer.h/.cpp` — Optimizer passes:
  - Predicate pushdown (filter → scan)
  - Projection pushdown (16 cols → 7 cols for Q1)
  - Physical operator selection
  - Pipeline decomposition (identify breaks: hash agg, sort)
- `src/optimizer/physical_plan_printer.h/.cpp` — Physical plan + pipeline printer
- `test/unit/test_optimizer.cpp` — Verify pushdown, column pruning, pipeline structure for Q1

**Design docs to update**: `doc/optimizer.md` if any rule changes

**Status**: [ ] Not Started

---

## Task 8: Tidy Tuples — Codegen API + SQL Values (Layers 5 & 4)

**Goal**: Typed IR builder wrapper and SQL value abstraction with NULL propagation.

**Deliverables**:
- `src/codegen/api/codegen_context.h/.cpp` — Layer 5: CodegenContext wrapping IRBuilder
  - Typed wrappers: Bool, Int8, Int32, Int64, UInt64, `Ptr<T>`
  - Control flow RAII: If, Loop, Function builders
  - Host function registration with type-safe signatures
  - Constant creation helpers
- `src/codegen/sql_values/sql_value.h/.cpp` — Layer 4: SQLValue = (value, null_flag, type)
  - Binary arithmetic: add/sub/mul/div with type promotion
  - Comparison: returns BOOLEAN SQLValue
  - NULL propagation: three-valued logic
  - DECIMAL scaling and rounding
  - Implicit cast emission
- `test/unit/test_codegen_api.cpp` — Codegen API: emit IR, run through interpreter, verify results
- `test/unit/test_sql_values.cpp` — SQL value operations, NULL propagation, DECIMAL arithmetic

**Design docs to update**: `doc/tidy-tuples.md` (Layers 4 & 5 sections)

**Status**: [ ] Not Started

---

## Task 9: Tidy Tuples — Tuples + Hash Table (Layers 3 & 2)

**Goal**: Tuple pack/unpack/hash and hash table codegen for aggregation.

**Deliverables**:
- `src/codegen/tuples/tuple_layout.h/.cpp` — Layer 3:
  - Layout computation: field offsets with alignment
  - Pack: write SQLValue list to memory buffer
  - Unpack: read back with null flags
  - Hash: CRC32-based hash combining
- `src/codegen/structures/hash_table.h/.cpp` — Layer 2:
  - Runtime hash table: open addressing, linear probing
  - Codegen interface: find-or-insert, iterate
  - Aggregate state layout alongside group keys
  - Resize/rehash logic
- `test/unit/test_tuples.cpp` — Pack/unpack round-trip, hash collision behavior
- `test/unit/test_hash_table.cpp` — Insert/lookup/iterate, aggregate accumulation patterns

**Design docs to update**: `doc/tidy-tuples.md` (Layers 2 & 3 sections)

**Status**: [ ] Not Started

---

## Task 10: Tidy Tuples — Operator Translators + Pipeline Execution (Layer 1)

**Goal**: Code generation for Q1 operators and pipeline execution framework.

**Deliverables**:
- `src/codegen/operators/operator_translator.h` — Base class with produce/consume protocol
- `src/codegen/operators/table_scan.h/.cpp` — TableScan: emit row loop, column loads, filter predicate
- `src/codegen/operators/hash_aggregate.h/.cpp` — HashAggregate:
  - Build side: hash group keys, find-or-insert, update SUM/COUNT states
  - Scan side: iterate entries, unpack, finalize AVG (SUM/COUNT)
- `src/codegen/operators/sort.h/.cpp` — Sort: materialize + qsort with comparison function
- `src/codegen/operators/projection.h/.cpp` — Projection: evaluate output expressions
- `src/execution/pipeline.h/.cpp` — Pipeline execution: compile IR → run function, pass RuntimeContext
- `src/execution/result_collector.h/.cpp` — Collect output rows for display
- `test/unit/test_operators.cpp` — Individual operator codegen + execution
- `test/ir/test_pipeline.cpp` — Full Q1 pipeline generation + interpreter execution

**Design docs to update**: `doc/tidy-tuples.md` (Layer 1), `doc/optimizer.md` (pipeline execution)

**Status**: [ ] Not Started

---

## Task 11: Flying Start JIT Backend

**Goal**: x86-64 JIT compiler with all four on-the-fly optimizations.

**Deliverables**:
- `src/backend/flying_start/live_span.h/.cpp` — Pass 0: RPO traversal, def/last-use tracking
- `src/backend/flying_start/translator.h/.cpp` — Pass 1: IR → x86-64 translation
  - Value location model: register / stack / flags / deferred
  - Opcode dispatch table for all ~63 opcodes
  - Function prologue/epilogue (frame pointer, callee-saved regs)
  - System V ABI for host function calls
- `src/backend/flying_start/register_alloc.h/.cpp` — 9-register allocation heuristic
- `src/backend/flying_start/optimizations.h/.cpp` — 4 optimizations:
  1. Stack space reuse (free on last use)
  2. Register allocation (spill/reload)
  3. Lazy address calculation (fold GEP into addressing modes)
  4. Comparison-branch fusion (defer cmp until condbr)
- `test/ir/test_flying_start.cpp` — Cross-validate interpreter vs JIT on all IR test programs
- `test/ir/test_jit_optimizations.cpp` — Verify each optimization produces correct results

**Design docs to update**: `doc/flying-start.md` if any translation rules change

**Status**: [ ] Not Started

---

## Task 12: EXPLAIN + Shell

**Goal**: Query inspection at all 7 levels and interactive REPL.

**Deliverables**:
- `src/explain/explain.h/.cpp` — EXPLAIN infrastructure:
  - Level enum: AST, LOGICAL, PHYSICAL, PIPELINE, IR, ASM, PROFILE, ALL
  - Dispatch to per-stage formatters
  - Timing measurement (plan, codegen, compile, execute)
- `src/shell/shell.h/.cpp` — REPL:
  - linenoise/replxx integration for line editing
  - Multi-line input (accumulate until `;`)
  - Dot commands: `.help`, `.quit`, `.tables`, `.schema`, `.timer`, `.backend`, `.load`, `.explain`
  - History persistence (`~/.umbra_history`)
  - Tab completion (keywords, table names, column names)
- `src/shell/formatter.h/.cpp` — Result formatting:
  - Box-drawing tables with auto-width columns
  - Right-align numbers, left-align strings/dates
- `src/main.cpp` — Full main() with REPL loop and non-interactive pipe mode
- `test/unit/test_explain.cpp` — Verify each EXPLAIN level produces output
- `test/unit/test_formatter.cpp` — Result formatting tests

**Design docs to update**: `doc/explain.md`, `doc/shell.md`

**Status**: [ ] Not Started

---

## Task 13: End-to-End Integration + TPC-H Q1

**Goal**: Full pipeline working, differential testing vs DuckDB, performance within target.

**Deliverables**:
- End-to-end integration: SQL string → parsed → bound → optimized → codegen → execute → results
- TPC-H SF=0.01 data bundled or generated via dbgen
- `test/sql/test_q1.cpp` — Q1 execution + result verification against DuckDB
- `test/sql/test_q1_variants.cpp` — INTERVAL variants (60/90/120 DAY), edge cases
- `test/perf/test_benchmark.cpp` — Latency measurement:
  - Breakdown: plan, codegen, compile, execute, total
  - Target: total < 3.1ms
- Performance tuning if needed (hot path optimization)
- Final documentation review: all docs consistent with implementation

**Design docs to update**: `doc/testing.md`, any docs with implementation deviations

**Status**: [ ] Not Started

---

## Progress Tracker

| Task | Description | Status | Commit | Notes |
|------|-------------|--------|--------|-------|
| 1 | Build System + Scaffold | [ ] Not Started | — | — |
| 2 | Storage Layer | [ ] Not Started | — | — |
| 3 | Umbra IR | [ ] Not Started | — | — |
| 4 | Interpreter Backend | [ ] Not Started | — | — |
| 5 | Parser | [ ] Not Started | — | — |
| 6 | Binder | [ ] Not Started | — | — |
| 7 | Optimizer | [ ] Not Started | — | — |
| 8 | Codegen API + SQL Values | [ ] Not Started | — | — |
| 9 | Tuples + Hash Table | [ ] Not Started | — | — |
| 10 | Operator Translators + Pipeline | [ ] Not Started | — | — |
| 11 | Flying Start JIT | [ ] Not Started | — | — |
| 12 | EXPLAIN + Shell | [ ] Not Started | — | — |
| 13 | E2E Integration + TPC-H Q1 | [ ] Not Started | — | — |

## Workflow Protocol

For each task:

1. **Main agent** spawns a **dev agent** with task spec + relevant design docs
2. Dev agent implements code + tests, runs build/lint/test/sanitizer/coverage checks
3. Dev agent signals completion to main agent
4. **Main agent** spawns a **review agent** to verify:
   - Full test suite passes (build, lint, test, sanitizer, coverage ≥ 90%)
   - Code quality and correctness review
   - Test coverage adequacy
   - Design doc consistency
5. If review **passes** → main agent commits, updates PLAN.md, moves to next task
6. If review **fails** → main agent sends dev agent back for rework (max 3 retries)
7. If 3 retries exhausted → notify developer for manual intervention
