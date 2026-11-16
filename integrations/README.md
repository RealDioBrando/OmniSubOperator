# Engine Integration Patches

These patches record the Phase 1 correctness integration and the Phase 2
single-group/error-policy checkpoint for the count micro-operator. They are
reference artifacts, not submodules and not patches against the
coworker-owned reference checkouts in the parent repository.

## Velox Spark count

- Combined Phase 2 patch: `velox/0001-combined-phase2.patch`
- Incremental Phase 2 patch: `velox/0002-add-single-group-shared-count-path.patch`
- Phase 1-only patch: `velox/0001-integrate-opt-in-omni-subop-count.patch`
- Base: `d313a8f1d` (`Ray0915/velox dev3`)
- Compile option: `VELOX_ENABLE_OMNI_SUBOP_COUNT=OFF` by default
- Source root: `-DOMNI_SUBOP_ROOT=<OmniSubOperator checkout>`
- Runtime switch: query config `omni_subop.count_enabled=false` by default
- Integrated methods: grouped and single-group raw-input/partial-merge hooks
- Left native: final extraction

The patch keeps the original implementation compiled and selectable. The
query-level test compares enabled and disabled execution for flat, dictionary,
constant, all-NULL, sparse-selection, raw, and partial-merge cases.

## OmniOperator grouped count

- Combined Phase 2 patch: `omniruntime/0001-combined-phase2.patch`
- Incremental Phase 2 patch: `omniruntime/0002-add-single-group-shared-count-path.patch`
- Phase 1-only patch: `omniruntime/0001-integrate-opt-in-omni-subop-count.patch`
- Base: `c6d813a3`
- Compile option: `OMNI_SUBOP_ENABLED=OFF` by default
- Source root: `-DOMNI_SUBOP_ROOT=<OmniSubOperator checkout>`
- Integrated methods: grouped and single-group raw count / partial merge
- Left native: spill merge and final extraction

The patch embeds `kernel/count_batch.cpp` and `kernel/version.cpp` directly in
`op_agg_obj`, avoiding a nested CMake project. The original code remains the
fallback and is still compiled.

## Validation recorded

- Velox: `velox_omni_subop_count_test` passed 2/2 tests, now including the
  global/single-group plan.
- OmniOperator: all 14 ON tests passed; the 13 common tests produce the same
  output with `OMNI_SUBOP_ENABLED=OFF`.
- Invalid state and overflow are fatal with `ON`, rather than falling back
  after a rejection.
- Both patches apply cleanly to their recorded base commits.
- No reference checkout was modified.

Performance benchmarking remains future work. Failure injection now covers invalid state and overflow before any write.