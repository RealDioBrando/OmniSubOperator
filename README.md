# OmniSubOperator

Shared, engine-independent micro-operators for native execution engines.

## Current scope

This branch contains the count reference implementation, its Phase 1-3 integration patches, and the first int64 SUM checkpoint:

- `kernel/count_batch.h` and `kernel/count_batch.cpp` — count kernel
- `kernel/sum_batch.h` — shared int64 SUM kernel
- `adapters/velox/CountBatch.h` and `adapters/velox/SumBatch.h` — Velox adapters
- `adapters/omniruntime/CountBatch.h` and `adapters/omniruntime/SumBatch.h` — OmniOperator adapters
- `tests/count_batch_test.cpp` and `tests/sum_batch_test.cpp` — dependency-free unit tests
- `integrations/velox/` — opt-in Velox Spark count and int64 SUM patches
- `integrations/omniruntime/` — opt-in OmniOperator count and int64 SUM patches

The adapters are header-only translation layers. The engine patches are kept
as review artifacts because the reference Velox and OmniOperator trees are
owned by other maintainers. The kernel owns no group table, vector decoding,
serialization, or final value extraction.

## Contract

The grouped kernel consumes normalized row masks and one engine-owned count
state pointer per row. The single-group adapter instead traverses engine-owned
rows once and supplies checked counters plus one delta. The kernel never reads
engine vectors.

- `active_rows == NULL` means all rows are selected.
- `nulls[row] != 0` means NULL. For raw count it is used only by `COUNT(expr)`.
- `row_counts[row]` points to the destination `int64_t` count state. The same
  pointer may repeat for rows in the same group.
- `count(*)` ignores the input NULL mask.
- `COUNT(expr)` counts selected, non-NULL rows.
- `merge_partial` skips NULL partial rows and adds non-negative partial values.
- All validation and overflow checks happen before the first accumulator write.
  On any non-OK status, no count state is modified.
- Single-group calls use `omni_subop_count_update_single_state`; they do not
  allocate normalized active, NULL, partial-value, or state-pointer arrays.

This is the Phase 0 fallback-safety guarantee. An error after a state write is
not a valid reason to rerun the same batch through a different implementation.

## Build and test

```bash
cmake -S . -B _build -DCMAKE_BUILD_TYPE=Release
cmake --build _build -j
ctest --test-dir _build --output-on-failure
```

Or:

```bash
BUILD_DIR=_build ./build.sh
```

Phase 2 was validated on `velox-10.3`. The Phase 3 single-state adapter was
validated on `velox-10.2`; both evidence bundles are recorded in the parent
repository.

Current integration status:

- Velox Spark count grouped and single-group hooks are opt-in and passed 2/2
  focused query tests with enabled/disabled comparisons.
- OmniOperator grouped and single-group raw/partial paths passed all 19 ON
  tests; the 18 common tests matched the OFF baseline, including filtered
  `COUNT(*)` skip-mask, spill/restore, mixed-state serde, and final-extraction
  coverage.
- A single shared count-state pointer now uses a map-free fast path.
- Single-group Velox and OmniOperator adapters no longer build per-row
  normalized vectors; their temporary state is O(1).
- Invalid state and overflow fail without fallback or state mutation.
- Count kernel tests pass 114/114 checks and int64 SUM kernel tests pass
  25/25 checks.
- SQL-level single-group and grouped filtered `COUNT(*)` produced identical
  ON/OFF result hashes; a temporary probe confirmed the adapter was entered
  with a non-NULL filter mask.
- Normal count spill/restore and mixed-state serialization are covered; spill
  overflow is guarded in the native merge loop.
- The first int64 SUM integration passes 2/2 Velox focused tests and 9/9
  OmniOperator tests in both ON and OFF configurations.
- The SUM grouped generic path is correctness-first; moving the existing
  SVE/bitmap grouped path into the shared kernel and extending mixed-state serde
  coverage remains open.

## Next steps

1. Apply the integration patches to maintainer-owned engine forks.
2. Move the Ray int64 SUM SVE/bitmap grouped path into the shared kernel.
3. Extend SUM spill, final extraction, and failure-injection coverage.
4. Benchmark int64 SUM on a quiet host and compare native, generic shared, and
   SVE paths.
5. Keep min/max deferred while the SUM path is stabilized; decimal SUM follows
   later.
