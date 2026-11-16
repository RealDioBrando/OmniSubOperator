# OmniSubOperator

Shared, engine-independent micro-operators for native execution engines.

## Current scope

This branch contains the Phase 0 aggregate-count prototype:

- `kernel/count_batch.h` — narrow C ABI for raw count and partial-count merge
- `kernel/count_batch.cpp` — validated count kernel
- `adapters/velox/CountBatch.h` — Velox row-state adapter
- `adapters/omniruntime/CountBatch.h` — OmniOperator row-state adapter
- `tests/count_batch_test.cpp` — dependency-free unit tests
- `CMakeLists.txt` — library and test build

The adapters are header-only translation layers. They are not wired into the
reference Velox or OmniOperator trees yet. The kernel owns no group table,
vector decoding, serialization, or final value extraction.

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

The standalone count test and the two adapter syntax checks have been run on
`velox-10.3`; the evidence is recorded in the parent repository.

## Next steps

1. Integrate the Velox raw-update and partial-merge calls behind a feature flag
   in a separate engine branch owned by the engine maintainer.
2. Integrate the OmniOperator `CountColumnAggregator` calls behind its existing
   compile-time switch.
3. Run enabled-versus-disabled query equivalence and forced-failure tests.
4. Benchmark only after correctness and fallback tests pass.
