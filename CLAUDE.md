# CLAUDE.md

## Project

UmbraDB MVP — a JIT-compiling OLAP database inspired by Umbra, targeting TPC-H Q1 (SF=0.01, single-threaded, total < 3.1ms).

- C++20, x86-64 Linux, CMake build system
- Design docs in `doc/`
- Task specs and workflow protocol in `PLAN.md`

### TPC-H Q1

```sql
SELECT l_returnflag, l_linestatus,
       SUM(l_quantity), SUM(l_extendedprice),
       SUM(l_extendedprice * (1 - l_discount)),
       SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax)),
       AVG(l_quantity), AVG(l_extendedprice), AVG(l_discount),
       COUNT(*)
FROM lineitem
WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
GROUP BY l_returnflag, l_linestatus
ORDER BY l_returnflag, l_linestatus;
```

### Performance Target

Reference (Tidy Tuples paper, SF=0.01, 1 thread): Umbra Flying Start total=2.19ms. Our target (70%): **< 3.1ms**.

## Architecture & Design Docs

```
Shell → SQL string → Parser → AST → Binder → Logical Plan → Optimizer → Physical Plan → Pipeline[]
  Per Pipeline: Tidy Tuples → Umbra IR → Interpreter or Flying Start (asmjit → x86-64) or LLVM (O3 + ORC JIT) → Result
  Cross-cutting: Storage (columnar), EXPLAIN (7 levels)
```

**Design docs are the source of truth.** Code changes that alter design must update the corresponding doc in the same commit. Agents: read the design doc and source for any component you touch.

| Component | Source | Design Doc | What triggers a doc update |
|-----------|--------|------------|---------------------------|
| Storage & Types | `src/storage/`, `src/catalog/`, `src/common/types.h` | `doc/storage.md` | Type representations, column format, data loading |
| Parser | `src/parser/` | `doc/parser.md` | AST node types, PG conversion logic |
| Binder | `src/binder/` | `doc/binder.md` | Binding rules, type inference, aggregate analysis |
| Optimizer | `src/optimizer/` | `doc/optimizer.md` | Optimization rules, physical plan, pipeline decomposition |
| Tidy Tuples | `src/codegen/` (operators/ structures/ tuples/ sql_values/ api/) | `doc/tidy-tuples.md` | 5-layer codegen interfaces, operator translators |
| Umbra IR | `src/ir/` | `doc/ir.md` | Opcodes, instruction encoding, builder, constant folding, DCE |
| Backends (overview) | `src/backend/`, `src/execution/` | `doc/backend/README.md` | Backend enum, unified interface, ABI |
| Interpreter | `src/backend/interpreter/` | `doc/backend/interpreter.md` | Execution model, value slots, PHI resolution |
| Flying Start | `src/backend/flying_start/` | `doc/backend/flying-start.md` | Register alloc, translation rules, optimizations |
| LLVM Backend | `src/backend/llvm/` | `doc/backend/llvm-backend.md` | IR lowering rules, LLVM optimization, ORC JIT |
| Inline Host Fns | `src/backend/llvm/` | `doc/backend/inline-host-functions.md` | LLVM host function inlining |
| Adaptive Execution | `src/execution/` | `doc/backend/adaptive-execution.md` | Morsel-driven dispatch, async LLVM, backend switching |
| EXPLAIN | `src/explain/` | `doc/explain.md` | EXPLAIN levels, output format |
| Shell | `src/shell/` | `doc/shell.md` | REPL commands, key bindings, result formatting |
| Morsel-Driven Parallel | `src/execution/` | `doc/morsel-driven-execution.md` | Thread pool, morsel dispatch, parallel aggregation, merge |
| Execution | `src/execution/` | `doc/execution.md` | Pipeline dispatch, RuntimeContext, result collection |
| Testing | `test/` (unit/ ir/ sql/ perf/) | `doc/testing.md` | Test strategy, categories, coverage approach |
| Overall | — | `CLAUDE.md` | Architecture, dependencies, scope |

### Scope Boundaries

**In Scope (MVP)**: TPC-H Q1 e2e, single-threaded, in-memory columnar, Interpreter + Flying Start, hash aggregation (SUM/AVG/COUNT), date/decimal arithmetic, basic EXPLAIN, differential testing vs DuckDB.

**Out of Scope**: Joins, subqueries, parallelism, disk storage, cost-based optimizer, LLVM, transactions, window functions, DML.

## Build & Verify

Quick build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`

Test: `cd build && ctest --output-on-failure`

**Full pre-commit verification** (format → build → lint → test → sanitizers → coverage):

```bash
./scripts/verify.sh            # all 6 steps
./scripts/verify.sh --skip-coverage  # faster iteration
```

## Commit Requirements

Every commit must pass `./scripts/verify.sh` (exit 0) before being created. This checks:

1. **Format**: `clang-format` applied to all `src/` and `test/` files
2. **Build**: Release build with `-Wall -Wextra -Werror`, zero warnings
3. **Lint**: `clang-tidy` with `--warnings-as-errors='*'`
4. **Test**: `ctest` exits 0
5. **Sanitizers**: all tests pass under ASan + UBSan
6. **Coverage**: line coverage ≥ 90%

Do not use `--no-verify` or skip any check.

### Reference

- **Tidy Tuples and Flying Start** (Kersten et al., VLDB 2020) → `reference/tidy-tuples.pdf`
- **TPC-H Choke Points** (Dreseler et al., PVLDB 2020) → `reference/tpch-choke-points.pdf`
