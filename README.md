# OmniSubOperator

Shared, engine-independent micro-operators for native execution engines.

## Current scope

This branch contains the count kernel and Phase 1-3 integration patches:

- `kernel/count_batch.h` — narrow C ABI for grouped and single-group count
- `kernel/count_batch.cpp` — validated count kernel
- `adapters/velox/CountBatch.h` — Velox row-state adapter
- `adapters/omniruntime/CountBatch.h` — OmniOperator row-state adapter
- `tests/count_batch_test.cpp` — dependency-free unit tests
- `integrations/velox/` — opt-in Velox Spark grouped/single-group count patches
- `integrations/omniruntime/` — opt-in OmniOperator grouped/single-group count patches

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
- OmniOperator grouped and single-group raw/partial paths passed all 16 ON
  tests; the 15 common tests matched the OFF baseline, including filtered
  `COUNT(*)` skip-mask coverage.
- A single shared count-state pointer now uses a map-free fast path.
- Single-group Velox and OmniOperator adapters no longer build per-row
  normalized vectors; their temporary state is O(1).
- Invalid state and overflow fail without fallback or state mutation.
- Standalone kernel tests pass 96/96 checks.
- Spill merge, SQL-level OmniOperator validation, and performance measurement
  are not implemented yet.

## Next steps

1. Apply the integration patches to maintainer-owned engine forks.
2. Add spill-path and final-extraction coverage.
3. Add SQL-level OmniOperator ON/OFF result-hash comparison.
4. Optimize grouped dense/sparse bitmap traversal.
5. Re-run ON/OFF equivalence, then benchmark on a quiet cluster.
