# Engine Integration Patches

These patches record the Phase 1 correctness integration for the count
micro-operator. They are reference artifacts, not submodules and not patches
against the coworker-owned reference checkouts in the parent repository.

## Velox Spark count

- Patch: `velox/0001-integrate-opt-in-omni-subop-count.patch`
- Base: `d313a8f1d` (`Ray0915/velox dev3`)
- Compile option: `VELOX_ENABLE_OMNI_SUBOP_COUNT=OFF` by default
- Source root: `-DOMNI_SUBOP_ROOT=<OmniSubOperator checkout>`
- Runtime switch: query config `omni_subop.count_enabled=false` by default
- Integrated methods: `addRawInput` and `addIntermediateResults`
- Left native: single-group methods and final extraction

The patch keeps the original implementation compiled and selectable. The
query-level test compares enabled and disabled execution for flat, dictionary,
constant, all-NULL, sparse-selection, raw, and partial-merge cases.

## OmniOperator grouped count

- Patch: `omniruntime/0001-integrate-opt-in-omni-subop-count.patch`
- Base: `c6d813a3`
- Compile option: `OMNI_SUBOP_ENABLED=OFF` by default
- Source root: `-DOMNI_SUBOP_ROOT=<OmniSubOperator checkout>`
- Integrated methods: grouped raw count and grouped partial merge
- Left native: single-group methods, spill merge, and final extraction

The patch embeds `kernel/count_batch.cpp` and `kernel/version.cpp` directly in
`op_agg_obj`, avoiding a nested CMake project. The original code remains the
fallback and is still compiled.

## Validation recorded

- Velox: `velox_omni_subop_count_test` passed 2/2 tests.
- OmniOperator: all 12 count-aggregator tests passed with
  `OMNI_SUBOP_ENABLED=ON` and again with `OMNI_SUBOP_ENABLED=OFF`.
- Both patches apply cleanly to their recorded base commits.
- No reference checkout was modified.

Performance and forced-failure testing remain future work.