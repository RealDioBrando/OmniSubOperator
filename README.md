# OmniSubOperator

Shared, engine-independent micro-operators for native execution engines.

## Current scope

This branch contains the Phase 0 count kernel and Phase 1 integration patches:

- `kernel/count_batch.h` — narrow C ABI for raw count and partial-count merge
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

The kernel consumes normalized row masks and one engine-owned count-state
pointer per row. It never reads engine vectors.

- `active_rows == NULL` means all rows are selected.
- `nulls[row] != 0` means NULL. For raw count it is used only by `COUNT(expr)`.
- `row_counts[row]` points to the destination `int64_t` count state. The same
  pointer may repeat for rows in the same group.
- `count(*)` ignores the input NULL mask.
- `COUNT(expr)` counts selected, non-NULL rows.
- `merge_partial` skips NULL partial rows and adds non-negative partial values.
- All validation and overflow checks happen before the first accumulator write.
  On any non-OK status, no count state is modified.

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

The standalone kernel, both adapters, and the two engine integration patches
were validated on `velox-10.3`; the evidence is recorded in the parent
repository.

Current integration status:

- Velox Spark count grouped and single-group hooks are opt-in and passed 2/2
  focused query tests with enabled/disabled comparisons.
- OmniOperator grouped and single-group raw/partial paths passed all 14 ON
  tests; the 13 common tests matched the OFF baseline.
- A single shared count-state pointer now uses a map-free fast path.
- Invalid state and overflow fail without fallback or state mutation.
- Spill merge and performance measurement are not implemented yet.

## Next steps

1. Apply the integration patches to maintainer-owned engine forks.
2. Add spill-path coverage.
3. Optimize dense/sparse bitmap traversal and partial-merge loops.
4. Re-run ON/OFF equivalence, then benchmark on a quiet cluster.
