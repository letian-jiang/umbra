# TPC-H Q1 Performance Profile

Benchmark of TPC-H Q1 across all UmbraDB backends and DuckDB, at SF=0.01 (60K rows) and SF=1 (6M rows), single-threaded.

## Environment

| | |
|---|---|
| CPU | AMD Ryzen 7 9700X 8-Core (WSL2) |
| RAM | 30 GB |
| OS | Linux 6.6.75 (WSL2) |
| Compiler | GCC 11.4, LLVM 14.0 |
| DuckDB | v1.3.2 |
| Morsel size | 10,000 (adaptive default) |

## Query

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

Pipeline decomposition:
- **Pipeline 0 (build)**: TableScan(filter) -> HashAggregate(build) — dominates total time
- **Pipeline 1 (scan)**: HashAggregate(scan) -> Projection -> Sort — negligible (<0.3ms)

## SF=0.01 (60,175 rows)

All times in milliseconds, 7 runs, median reported.

### Phase Breakdown

| Backend | Plan | Codegen | Compile | Execute | **Total** |
|---------|------|---------|---------|---------|-----------|
| Interpreter | 0.25 | 0.04 | 0.06 | 24.8 | **25.1** |
| Flying Start | 0.24 | 0.03 | 0.22 | 1.16 | **1.71** |
| LLVM | 0.24 | 0.04 | 16.8 | 0.21 | **17.4** |
| Adaptive | 0.24 | 0.04 | 0.39 | 1.15 | **18.4** |
| **DuckDB (1T)** | — | — | — | — | **2.0** |
| DuckDB (16T) | — | — | — | — | 3.0 |

### Analysis (SF=0.01)

At 60K rows, data fits in L2 cache (~0.5MB per column, 7 columns = 3.4MB).

- **Flying Start wins** at 1.71ms. Compile is instant (0.22ms), execution is fast.
- **LLVM is bottlenecked by compile** (16.8ms) — execution is only 0.21ms (6x faster than FS) but compile cost dominates.
- **Adaptive = FS + LLVM async overhead**: only 7 morsels total, LLVM never finishes in time. All morsels run via FS. The 18.4ms total includes waiting for the LLVM async thread to clean up.
- **DuckDB** at 2.0ms (1 thread) is close to Flying Start. DuckDB uses vectorized interpretation, not JIT.

### Execution Rate Comparison (Pipeline 0 only)

| Backend | Execute (ms) | ns/row | vs Interpreter |
|---------|-------------|--------|----------------|
| Interpreter | 24.8 | 412 | 1x |
| Flying Start | 1.16 | 19.3 | **21x** |
| LLVM | 0.21 | 3.5 | **118x** |

## SF=1 (6,001,215 rows)

All times in milliseconds, 7 runs, median reported.

### Phase Breakdown

| Backend | Plan | Codegen | Compile | Execute | **Total** |
|---------|------|---------|---------|---------|-----------|
| Interpreter | 0.25 | 0.04 | 0.07 | 2,490 | **2,491** |
| Flying Start | 0.25 | 0.04 | 0.26 | 116 | **116** |
| LLVM | 0.25 | 0.04 | 18.2 | 21.5 | **40** |
| Adaptive | 0.25 | 0.04 | 0.43 | 33.2 | **39.7** |
| **DuckDB (1T)** | — | — | — | — | **135** |
| DuckDB (16T) | — | — | — | — | 18 |

### Adaptive Breakdown (SF=1)

| Phase | Time | Detail |
|-------|------|--------|
| FS compile | 0.27ms | asmjit single-pass |
| LLVM async compile | 13.6ms | O3 + ORC JIT (background) |
| FS execution | ~13ms | 71 morsels (710K rows) |
| LLVM execution | ~20ms | 530 morsels (5.3M rows) |
| Switch point | morsel 71 | ~13ms after start |
| **Total execute** | **33.2ms** | |

### Analysis (SF=1)

At 6M rows, column data totals ~336MB (far exceeds all caches).

- **Adaptive is fastest end-to-end** (39.7ms), slightly beating pure LLVM (40ms). FS's instant compile (0.27ms) hides LLVM's 13.6ms compile latency by executing morsels immediately.
- **LLVM execution is 5.4x faster than FS** (21.5 vs 116ms), thanks to O3 optimization and host function inlining.
- **Flying Start beats DuckDB single-threaded** (116 vs 135ms). DuckDB's vectorized interpretation is comparable to our single-pass JIT.
- **DuckDB multi-threaded** (18ms, 16 threads) is faster than our single-threaded execution. Parallel execution is out of scope for this MVP.

### Execution Rate Comparison (Pipeline 0 only)

| Backend | Execute (ms) | ns/row | vs Interpreter |
|---------|-------------|--------|----------------|
| Interpreter | 2,490 | 415 | 1x |
| Flying Start | 116 | 19.3 | **21x** |
| LLVM | 21.5 | 3.6 | **116x** |
| DuckDB (1T) | 135 | 22.5 | 18x |

### Compile vs Execute Tradeoff

| Backend | Compile | Execute | Compile % | First-row latency |
|---------|---------|---------|-----------|-------------------|
| Interpreter | 0.07ms | 2,490ms | 0% | 0.07ms |
| Flying Start | 0.26ms | 116ms | 0.2% | 0.26ms |
| LLVM | 18.2ms | 21.5ms | 46% | 18.2ms |
| Adaptive | 0.43ms | 33.2ms | 1.3% | **0.27ms** |

Adaptive achieves the best of both worlds: near-instant first-row latency (0.27ms) with near-LLVM execution speed. The 13.6ms LLVM compile happens entirely in the background while FS processes early morsels.

## Scaling: SF=0.01 vs SF=1 (100x data)

| Backend | SF=0.01 | SF=1 | Ratio | Ideal (100x) |
|---------|---------|------|-------|---------------|
| Interpreter | 25ms | 2,491ms | 100x | 100x |
| Flying Start | 1.71ms | 116ms | 68x | 100x |
| LLVM | 17.4ms | 40ms | 2.3x | 100x |
| Adaptive | 18.4ms | 39.7ms | 2.2x | 100x |

- **Interpreter scales linearly** (pure interpretation, no fixed costs).
- **Flying Start** scales sub-linearly (68x) because the 0.22ms compile cost is amortized.
- **LLVM/Adaptive** scale very sub-linearly (2.3x) because compile cost (~17ms) dominates at SF=0.01 but is amortized at SF=1.

## Summary

```
                    SF=0.01                 SF=1
                    total    execute        total    execute
Interpreter:        25.1ms   24.8ms         2491ms   2490ms
Flying Start:        1.7ms    1.2ms          116ms    116ms    <-- best for small data
LLVM:               17.4ms    0.2ms           40ms     22ms
Adaptive:           18.4ms    1.2ms           40ms     33ms    <-- best for large data
DuckDB (1T):         2.0ms                   135ms             <-- vectorized interp
DuckDB (16T):        3.0ms                    18ms             <-- parallel execution
```

**Key takeaways:**
1. Flying Start is optimal for small datasets (< ~100K rows) where compile overhead matters.
2. LLVM/Adaptive dominate at scale, with 5x faster execution than Flying Start.
3. Adaptive matches LLVM end-to-end while providing 67x lower first-row latency.
4. Our single-threaded LLVM execution (3.6 ns/row) approaches DuckDB's parallel throughput on a per-core basis.
