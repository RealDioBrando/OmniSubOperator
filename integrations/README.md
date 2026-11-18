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
- Decimal SUM integration: `velox/0009-decimal-sum-integration.patch`
- Int64 min/max integration: `velox/0011-int64-minmax-integration.patch`
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
- Decimal SUM integration: `omniruntime/0009-decimal-sum-integration.patch`
- Int64 min/max integration: `omniruntime/0012-int64-minmax-integration.patch`
- Focused decimal tests: `omniruntime/0010-decimal-sum-omni-test.patch`
- Focused min/max tests: `omniruntime/0013-int64-minmax-test.patch`
- Mixed-state SUM serde test: `omniruntime/0008-sum-mixed-state-serde.patch`
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
- Int64 SUM kernel tests passed 57/57 after the packed/SVE checkpoint.
  OmniOperator SUM tests passed 10/10 ON and 10/10 OFF, including the
  `EMPTY` / `NORMAL` / `OVERFLOWED` mixed-state serde case.
- Decimal SUM kernel tests passed 28/28; Velox decimal integration passed 3/3
  focused checks; OmniOperator decimal tests passed 3/3 ON and 3/3 OFF.
- Int64 min/max kernel tests passed 15/15; Velox passed 2/2 focused checks;
  OmniOperator passed 2/2 ON and 2/2 OFF.
- Count kernel tests pass 114/114 checks; int64 SUM kernel tests pass 57/57
  checks.
- Invalid state and overflow are fatal with `ON`, rather than falling back
  after a rejection.
- Combined and incremental patches apply cleanly to their recorded bases.
- No reference checkout was modified.

The decimal/min/max checkpoint and benchmark are recorded in
`docs/evidence/2026-11-18-decimal-minmax/README.md`. Decimal SUM and int64
min/max are correctness-first callback paths; packed/bitmap hardening, decimal
partial merge, full SVE gather/scatter, and engine-level failure injection remain
future work.
