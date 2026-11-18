# Engine Integration Patches

These files record the OmniSubOperator engine integrations. The canonical
Velox artifact is now the pre-Ray migration patch; the earlier Velox patches
are retained only as historical checkpoints.

## Canonical Velox migration

- Patch: `velox/0100-pre-ray-aggregate-migration.patch`
- Base: `ced070fdb` (the commit before Ray's four in-scope aggregate changes)
- Apply from a clean base checkout:

  ```bash
  patch -p1 < integrations/velox/0100-pre-ray-aggregate-migration.patch
  ```

- Build switches:
  - `VELOX_ENABLE_OMNI_SUBOP_COUNT=ON`
  - `VELOX_ENABLE_OMNI_SUBOP_SUM=ON`
  - `VELOX_ENABLE_OMNI_SUBOP_MINMAX=ON`
  - `OMNI_SUBOP_ROOT=<OmniSubOperator checkout>`
- Runtime switches:
  - `omni_subop.count_enabled`
  - `omni_subop.sum_enabled`
  - `omni_subop.minmax_enabled`

All three compile switches default to OFF and all three runtime switches default
to false.

This patch is a **replacement migration**. It does not include Ray's optimized
count, min/max, `SUM`, decimal `SUM`, bitmap, dense-loop, or mode-getter code.
Ray's reference tree is used for behavior, measurements, and migration evidence
only. The pre-Ray generic implementation remains as the OFF baseline and as a
no-partial-write fallback for unsupported shapes.

Integrated Velox methods:

- Count: grouped/single-group raw input and partial merge;
- int64 `SUM`: grouped/single-group raw input and grouped/single-group partial
  merge;
- Decimal `SUM`: grouped raw input and grouped/single-group partial merge;
- int64 min/max: grouped raw input; partial merge reuses the same raw path.

Final extraction and other unchanged engine lifecycle methods remain in
Velox.

## Historical Velox checkpoints

The following patches were built against `d313a8f1d` (`Ray0915/velox dev3`)
and are retained for review history:

- `velox/0002-combined-phase3.patch`
- `velox/0003-single-state-adapter.patch`
- `velox/0006-grouped-state-base-hardening.patch`
- `velox/0007-int64-sum-integration.patch`
- `velox/0009-decimal-sum-integration.patch`
- `velox/0011-int64-minmax-integration.patch`
- `velox/0012-decimal-sum-partial-merge.patch`
- `velox/0001-combined-phase2.patch`
- `velox/0002-add-single-group-shared-count-path.patch`
- `velox/0001-integrate-opt-in-omni-subop-count.patch`

They are not the final migration state and must not be used to reconstruct the
product Velox tree. In particular, applying the old patches to Ray's branch
would leave Ray's optimized implementations compiled alongside the shared path.

## OmniOperator integrations

### OmniOperator count

- Combined Phase 3 patch: `omniruntime/0002-combined-phase3.patch`
- Incremental Phase 3 patch: `omniruntime/0003-single-state-adapter.patch`
- Filtered `COUNT(*)` follow-up: `omniruntime/0004-filtered-count-star-fix.patch`
- Phase 3 closure regressions: `omniruntime/0005-spill-restore-tests.patch`
- Count grouped-state hardening: `omniruntime/0006-grouped-state-base-hardening.patch`
- Int64 SUM integration: `omniruntime/0007-int64-sum-integration.patch`
- Decimal SUM integration: `omniruntime/0009-decimal-sum-integration.patch`
- Int64 min/max integration: `omniruntime/0012-int64-minmax-integration.patch`
- Spark decimal raw/partial shared paths: `omniruntime/0014-decimal-spark-shared-paths.patch`
- Spark decimal partial merge test: `omniruntime/0015-decimal-partial-omni-test.patch`
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

The OmniOperator patch embeds `kernel/count_batch.cpp` and
`kernel/version.cpp` directly in `op_agg_obj`, avoiding a nested CMake project.
The original OmniOperator code remains the fallback and is still compiled.

## Validation recorded

Final pre-Ray Velox validation, `velox-10.3`:

- Count integration: 2/2 tests passed;
- int64/decimal `SUM` integration: 4/4 tests passed;
- int64 min/max integration: 3/3 tests passed;
- Focused CTest executables: 3/3 passed;
- Count kernel: 114/114 on SVE and 114/114 on non-SVE;
- int64 `SUM` kernel: 63/63 on SVE and 59/59 on non-SVE;
- Decimal `SUM` kernel: 78/78 on SVE and 71/71 on non-SVE;
- int64 min/max kernel: 39/39 on SVE and 35/35 on non-SVE.

The counts include the three later state-base no-write regression checks.

The new decimal checks include ordinary negative addition and group NULL
marking. Earlier OmniOperator evidence remains recorded in
`docs/evidence/2026-11-17-aggregate-count-hardening/`,
`docs/evidence/2026-11-18-decimal-minmax/`, and
`docs/evidence/2026-11-18-packed-hardening/`. The final replacement migration
is recorded in `docs/evidence/2026-11-18-pre-ray-migration/`. Hardening is
recorded in `docs/evidence/2026-11-18-aggregate-hardening/`.

Hardening checkpoint:

- Debug failure injection passed for count, int64 `SUM`, decimal `SUM`, and
  int64 min/max: injected fallback statuses were applied once and injected
  post-adapter errors were fatal.
- Release/Debug test inventories: count 3/3 and 5/5; int64/decimal `SUM` 6/6
  and 10/10; int64 min/max 4/4 and 6/6.
- Coverage now includes inherited integer SUM types, varchar count/min/max,
  multiple batches, decimal overflow, all-NULL groups, filtered `COUNT(*)`, and
  the spill/local-exchange/table-scan plans generated by `AggregationTestBase`.
- The benchmark uses one million rows, 64 groups, and seven repeats. It covers
  both in-memory values and a DWRF file split into four Hive splits with four
  parallel drivers. The ON/OFF ratios (`off/on`; below 1 means ON is slower)
  were 0.723, 0.749, 0.613, and 0.891 for values, and 0.648, 0.608, 0.755,
  and 0.926 for the four-driver scan path. All defaults therefore remain OFF.

State-base refactor checkpoint:

- Direct `groups + stateOffset` paths now borrow active/NULL bits for flat or
  constant int64 SUM, min/max, and decimal raw input.
- The production direct paths rely on the engine invariant that every active,
  non-NULL row has a valid group base. Untrusted views prevalidate all such
  bases before the first accumulator write, so a later null base cannot leave a
  partially updated batch.
- Dictionary and irregular encodings retain the normalized packed fallback.
- Post-refactor ON/OFF ratios (`off/on`): values count 0.726, int64 SUM 1.273,
  decimal SUM 0.737, min/max 1.279; four-driver scan count 0.645, int64 SUM
  0.854, decimal SUM 0.808, min/max 1.214.
- Int64 SUM and min/max now beat OFF in memory; min/max also beats OFF on the
  four-driver scan. Performance remains mixed overall, so defaults remain OFF.

Earlier integration behavior:

- Filtered `COUNT(*)` raw semantics are covered in grouped and single-group
  tests. A non-NULL skip mask disables count-all mode so rejected rows are not
  counted.
- Spill/restore, mixed-state serialization, and final extraction are covered by
  `0005-spill-restore-tests.patch`. The spill merge loop remains native.
- SQL-level single-group and grouped filtered `COUNT(*)` produced identical
  ON/OFF result hashes. A temporary probe confirmed native adapter entry with a
  non-NULL filter mask; the probe was removed before the final build.
- OmniOperator SUM tests passed 10/10 ON and 10/10 OFF, including the
  `EMPTY` / `NORMAL` / `OVERFLOWED` mixed-state serde case.
- OmniOperator decimal tests passed 4/4 ON and 4/4 OFF, including the
  two-column partial merge test.
- OmniOperator min/max tests passed 2/2 ON and 2/2 OFF.
- Invalid state and overflow are fatal with `ON`, rather than falling back
  after a rejection.
- No coworker-owned reference checkout was modified.

## Ray aggregate migration audit

- Audit: `docs/en/designs/omni-suboperator/ray-optimization-shipping-audit.md`
- In scope: count, int64 min/max, int64 `SUM`, decimal `SUM`
- Out of scope: MD5/`concat_ws` and ISA-L
- Final Velox model: pre-Ray generic code plus shared OmniSubOperator paths;
  Ray's optimized Velox implementations are removed, not retained as fallback.
