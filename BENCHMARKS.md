# Benchmarks

Two suites:

1. **`bench/compare.c`** — one binary per allocator, identical workloads,
   min of three runs, ns per alloc+free pair. This is the table in the
   README. Graphs: `bench/images/compare-ns.png`.
2. **[mimalloc-bench](https://github.com/daanx/mimalloc-bench)** — the
   academic/real-program suite rpmalloc publishes (`allt`). Memento is
   wired in as an `LD_PRELOAD` shim via `--external`, the same way you
   would drop `memento_malloc.so` into a production process. Graphs:
   `bench/images/allt-time.png` and `allt-rss.png` (generated when a
   captured CSV is present).

`python3 bench/plot.py` regenerates every graph from `bench/results/*.csv`.
It needs only matplotlib. Allocators more than 4× slower than memento on
a given test are omitted from that chart; the CSVs stay complete.

## Machine

The captured numbers below were collected on:

- NVIDIA Tegra, Cortex-A78AE, 6 cores, aarch64
- 7.4 GiB RAM
- Ubuntu 22.04, kernel 5.15.148-tegra, glibc 2.35
- `cc` is Clang 22.1.3; binaries built `-O3 -march=native -DNDEBUG`
- CPU governor: schedutil (max 1.73 GHz)

Absolute nanoseconds are a function of this 1.7 GHz core. Read the
**ordering**, not the absolute values, against an x86 desktop.

## compare.c (min of 3)

![compare.c ns/op](bench/images/compare-ns.png)

| workload | memento 3.0 | 3.0 `STATS=0` | mimalloc 2.1.7 | rpmalloc 1.4.5 | glibc |
|---|---:|---:|---:|---:|---:|
| churn 64 B | 21.3 | 14.8 | 19.8 | **9.0** | 21.6 |
| churn 1 KiB | 21.2 | 14.7 | 25.3 | **12.1** | 21.9 |
| bulk 64 B | 49.5 | 41.7 | **28.9** | 45.0 | 81.9 |
| bulk 4 KiB | 690 | 683 | **584** | 1045 | 1958 |
| mixed 16–256 B | 100 | 93.4 | **28.3** | 33.8 | 91.1 |
| churn 16 KiB | 27.3 | 22.0 | 39.0 | **18.4** | 74.2 |
| 4-thread churn 64 B | 6.0 | 3.6 | 5.6 | **2.5** | 5.9 |

How to read it:

- **Hit-path churn.** `STATS=0` is the fair product comparison (the
  default build still carries the counters the diagnostic story wants).
  rpmalloc remains ahead (~9 vs ~15 ns). `STATS=0` beats mimalloc and
  glibc on this core.
- **Mixed sizes** is still the gap: about 3× mimalloc (93 vs 28), down
  from 4–5×. A fresh page bump-carves without writing the payload;
  recycled blocks travel the page freelist; pages are reset only when
  claimed. The STATS=1 mixed row claims ~118 pages (the live payload)
  rather than 31 per span. The leftover is a cold 11-class burst.
- **Bulk 64 B / 4 KiB.** 2 MiB-aligned VA lets THP actually collapse:
  bulk-4 KiB sits next to mimalloc (690 vs 584 ns) and ahead of
  rpmalloc. bulk-64 B (`STATS=0` 41.7) now beats rpmalloc (45) and
  trails mimalloc (29) — those keep a hotter unbounded freelist; we
  recycle at span grain with a bounded tcache (depth 32).
- **16 KiB churn** is the page-run cache: memento beats mimalloc and
  glibc, trails rpmalloc.
- **Four threads.** Same shape as single-thread churn, scaled. No
  cross-thread free in this harness (see larson / rptest in the
  mimalloc-bench section for bleeding).

Reproduce:

```
./bench/run_compare.sh          # 3 runs, min, CSV + PNG
python3 bench/plot.py           # regenerate graphs from captured CSV
```

## mimalloc-bench

Memento is not a first-class allocator in upstream mimalloc-bench. The
hook is the one they already ship: `bench.sh --external=<file>` with
`name /path/to.so` per line. `./bench/run_mimalloc_bench.sh` clones the
suite into gitignored `extern/mimalloc-bench`, builds `memento_malloc.so`
plus shared mimalloc/rpmalloc, and runs a subset that does not need
sudo, ninja, redis, lean, or a 5 000-page PDF:

```
./bench/run_mimalloc_bench.sh
# override the test list:
TESTS="cfrac espresso alloc-test rptest" ./bench/run_mimalloc_bench.sh
```

That writes `bench/results/mimalloc-bench-allt.csv` in the
`benchres.csv` layout (`benchmark allocator elapsed rss user sys
page-faults page-reclaims`) and refreshes the allt graphs.

To run the full `allt` set (lean, redis, rocksdb, ghostscript, every
allocator mimalloc-bench knows), use their own builder — it wants
packages and a lot of RAM:

```
cd extern/mimalloc-bench
./build-bench-env.sh all          # sudo, downloads, hours
cd out/bench
../../bench.sh --external=../../bench/results/external.allocs \
    alla allt
cp benchres.csv /path/to/memento/bench/results/mimalloc-bench-allt.csv
python3 /path/to/memento/bench/plot.py
```

`me` is memento, `me0` is `MEMENTO_STATS=0`, `mi` / `rp` / `sys` are
the same three we compare in `compare.c`. Isolated crashes are missing
data points, not zeros; an allocator that fails more than two tests is
dropped from the graphs (rpmalloc's 4× rule, applied in `plot.py`).
A result more than 4× *faster* than the median is also treated as a
crash (the process died before doing the work).

### This machine's subset run

![mimalloc-bench elapsed](bench/images/allt-time.png)
![mimalloc-bench RSS](bench/images/allt-rss.png)

The `LD_PRELOAD` shim (sized malloc, 16-byte header) is a different
API than `compare.c`'s exact `thread_heap_alloc`. On the tests that
completed, `me0` is typically 1.3–2.5× mimalloc/rpmalloc and uses more
RSS — `glibc-thread` in particular (368 MiB vs 9–27 MiB) because parked
spans stay mapped. `malloc-large` matches rpmalloc (both ~7.4 s) and
trails mimalloc.

The shim is **not allt-clean yet**. `memento_free` is pagemap-first
now (16-bit magic is a canary, not the gate); smashed-header unit
tests pass. Remaining skips:

| test | why |
|---|---|
| espresso | `MEMENTO_DEBUG`: `memento_free` of a pointer not in the pagemap. Release SIGSEGV in `sm_row_dominance`. Interior/non-malloc free after realloc, still open. |
| mstressN | `MEMENTO_DEBUG`: double-free cookie. Cross-thread sized free still double-accounts a block. |
| rptestN / larsonN `me` | same family; `me0` completes larson. |
| barnes | everyone fails (missing input). Skip-with-reason. |

`me` is omitted from the graphs when it fails more than two tests; the
CSV still has the raw rows.

A VA-bump bug bit the first shim run: `va_bump` aligned the *offset*
into the reservation instead of the resulting address, so spans were
not 2 MiB-aligned, `memento_span_of` AND'd into `PROT_NONE`, and
sized `free` SIGSEGV'd. Exact-API `compare.c` never hit that (it uses
`page_of`). Fixed; `tests/test_sized.c` now asserts a live span header.

**Hit path is now in the public header.** `memento_thread_heap_alloc` /
`_free` FORCE_INLINE tcache pop/push in every TU; miss path is
`memento_fast_alloc_miss` / `memento_free_size_class_miss`. A consumer
`.c` that only `#include "memento.h"` no longer pays call/ret on churn.

**Shim RSS:** `memento_malloc.c` compiles with `SPAN_CACHE_MAX=2` and
`SPAN_PURGE_MS=0`. Park purges immediately. `malloc_trim` DONTNEEDs
unused pages in live spans (keeps the hot page). Decay ticks every 64
misses.

**Mixed is not claim-dominated.** Quick `compare.c` counters on this box
(12 500 pairs): `tcache=10536 page=1575 claim_page=33 claim_span=10`.
Deepening the tcache will not move that row. Leave geometry; the gap
is elsewhere (class-walk / first-touch / sized header), not page-claim.
