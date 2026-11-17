# Engine Integration Patches

These patches record the Phase 1 correctness integration, the Phase 2
single-group/error-policy checkpoint, and the Phase 3 allocation-free
single-group adapter. They are reference artifacts, not submodules and not
patches against the coworker-owned reference checkouts in the parent
repository.

## Velox Spark count

- Combined Phase 3 patch: `velox/0002-combined-phase3.patch`
- Incremental Phase 3 patch: `velox/0003-single-state-adapter.patch`
- Count grouped-state hardening: `velox/0006-grouped-state-base-hardening.patch`
- Int64 SUM integration: `velox/0007-int64-sum-integration.patch`
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
constant, all-NULL, sparse-selection, grouped, single-group, raw, and
partial-merge cases. The Phase 3 single-group adapter removes per-batch
normalized vectors.

## OmniOperator count

- Combined Phase 3 patch: `omniruntime/0002-combined-phase3.patch`
- Incremental Phase 3 patch: `omniruntime/0003-single-state-adapter.patch`
- Filtered `COUNT(*)` follow-up: `omniruntime/0004-filtered-count-star-fix.patch`
- Phase 3 closure regressions: `omniruntime/0005-spill-restore-tests.patch`
- Count grouped-state hardening: `omniruntime/0006-grouped-state-base-hardening.patch`
- Int64 SUM integration: `omniruntime/0007-int64-sum-integration.patch`
- SQL reproduction query: `omniruntime/validation/filtered-count-star.sql`
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
  global/single-group plan and sparse single-group selection. The Velox SUM
  integration passed 2/2 focused tests.
- OmniOperator: all 19 ON tests passed; the 18 common tests produce the same
  output with `OMNI_SUBOP_ENABLED=OFF`.
- Filtered `COUNT(*)` raw semantics are covered in grouped and single-group
  tests. A non-NULL skip mask disables count-all mode so rejected rows are not
  counted.
- Spill/restore, mixed-state serialization, and final extraction are covered by
  `0005-spill-restore-tests.patch`. The spill merge loop remains native.
- SQL-level single-group and grouped filtered `COUNT(*)` produced identical
  ON/OFF result hashes. A temporary probe confirmed native adapter entry with a
  non-NULL filter mask; the probe was removed before the final build.
- Int64 SUM kernel tests passed 25/25. OmniOperator SUM tests passed 8/8 ON and
  8/8 OFF; the shared path preserves Spark wrapping-overflow semantics.
- Count kernel tests pass 114/114 checks; int64 SUM kernel tests pass 25/25
  checks.
- Invalid state and overflow are fatal with `ON`, rather than falling back
  after a rejection.
- Combined and incremental patches apply cleanly to their recorded bases.
- No reference checkout was modified.

Performance benchmarking, grouped bitmap/vector optimization, SUM SVE
migration, SUM spill/final coverage, and broader SQL coverage remain future
work. Current count failure injection covers invalid state and overflow before
any write.
