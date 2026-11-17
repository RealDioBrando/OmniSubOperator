/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Narrow C ABI for the count micro-operator.
 *
 * Grouped calls use normalized row masks and per-row engine-state handles.
 * Single-group calls may instead provide adapter-classified row counts and a
 * pre-aggregated delta. In both cases the engine adapter owns vector decoding,
 * group lookup, and accumulator memory; the kernel never sees engine types.
 */

#ifndef OMNI_SUBOP_COUNT_BATCH_H
#define OMNI_SUBOP_COUNT_BATCH_H

#include <stdint.h>

#include "export.h"
#include "version.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OmniSubOpCountStatus {
    OMNI_SUBOP_COUNT_OK = 0,
    OMNI_SUBOP_COUNT_INVALID_ARGUMENT = -1,
    OMNI_SUBOP_COUNT_UNSUPPORTED_ENCODING = -2,
    OMNI_SUBOP_COUNT_INVALID_STATE = -3,
    OMNI_SUBOP_COUNT_OVERFLOW = -4,
    OMNI_SUBOP_COUNT_INTERNAL_ERROR = -5
} OmniSubOpCountStatus;

typedef enum OmniSubOpCountMode {
    OMNI_SUBOP_COUNT_ALL = 0,
    OMNI_SUBOP_COUNT_EXPR = 1
} OmniSubOpCountMode;

/* These are normalized shapes, not engine-specific encodings. Count does not
 * need the value bytes, but the field makes the adapter boundary explicit and
 * lets tests assert that all supported shapes have identical count semantics. */
typedef enum OmniSubOpValueEncoding {
    OMNI_SUBOP_VALUE_FLAT = 0,
    OMNI_SUBOP_VALUE_CONSTANT = 1,
    OMNI_SUBOP_VALUE_DICTIONARY = 2
} OmniSubOpValueEncoding;

typedef struct OmniSubOpCountResult {
    uint32_t abi_version;
    uint32_t groups_touched;
    uint64_t selected_rows;
    uint64_t counted_rows;
    uint64_t null_skipped_rows;
    uint64_t reserved;
} OmniSubOpCountResult;

/*
 * Raw count input.
 *
 * active_rows: NULL means every row is active; non-zero means selected.
 * nulls: 1 means NULL. It is used by COUNT(expr) and ignored by COUNT(*).
 * row_counts: one engine-owned count-state pointer per row. Pointers may
 *             repeat when multiple rows belong to the same group. Every
 *             active row must have a non-null pointer.
 * encoding: normalized input shape; count semantics are identical for all
 *           supported values.
 *
 * On any non-OK return, no count state is modified.
 */
typedef struct OmniSubOpCountArgs {
    uint32_t abi_version;
    int32_t num_rows;
    OmniSubOpCountMode mode;
    OmniSubOpValueEncoding encoding;
    const uint8_t* active_rows;
    const uint8_t* nulls;
    int64_t* const* row_counts;
} OmniSubOpCountArgs;

/*
 * Intermediate-result merge input.
 *
 * nulls: 1 means a NULL partial value; NULL means no partial nulls.
 * partial_counts is the engine-normalized count value for each selected row.
 * row_counts is the same per-row group-state view used by the raw path.
 * The kernel skips NULL partial rows and never writes their state.
 *
 * On any non-OK return, no count state is modified.
 */
typedef struct OmniSubOpCountMergeArgs {
    uint32_t abi_version;
    int32_t num_rows;
    const uint8_t* active_rows;
    const uint8_t* nulls;
    const int64_t* partial_counts;
    int64_t* const* row_counts;
} OmniSubOpCountMergeArgs;

/*
 * Pre-aggregated single-state update.
 *
 * The adapter has already traversed its engine-owned rows, classified each
 * selected row as counted or NULL, and summed non-NULL partial values. This
 * entry point keeps the final counter arithmetic and no-partial-write
 * overflow check in the shared kernel without requiring normalized row arrays.
 *
 * selected_rows must equal counted_rows + null_skipped_rows. delta must be
 * non-negative. state may be NULL only when counted_rows is zero. On any
 * non-OK return, the count state is not modified.
 */
typedef struct OmniSubOpCountSingleStateArgs {
    uint32_t abi_version;
    uint32_t reserved;
    int64_t* state;
    uint64_t selected_rows;
    uint64_t counted_rows;
    uint64_t null_skipped_rows;
    int64_t delta;
} OmniSubOpCountSingleStateArgs;

/*
 * Grouped input using engine-owned row bases.
 *
 * row_bases[row] is the engine's base address for one group state. The count
 * accumulator is located at row_bases[row] + state_offset. This lets engines
 * pass their existing group/row-state pointer array directly instead of
 * constructing a second pointer array through a callback.
 *
 * active_rows has the same meaning as in OmniSubOpCountArgs. nulls is used
 * only for COUNT(expr) or a FILTER skip mask. The adapter must not pass a
 * compacted row_bases array together with a row-indexed active_rows mask.
 */
typedef struct OmniSubOpCountGroupedArgs {
    uint32_t abi_version;
    int32_t num_rows;
    OmniSubOpCountMode mode;
    uint32_t state_offset;
    const uint8_t* active_rows;
    const uint8_t* nulls;
    uint8_t* const* row_bases;
} OmniSubOpCountGroupedArgs;

/*
 * Grouped partial-result merge using engine-owned row bases.
 * partial_counts is indexed by row and contains the engine-normalized count.
 */
typedef struct OmniSubOpCountGroupedMergeArgs {
    uint32_t abi_version;
    int32_t num_rows;
    uint32_t state_offset;
    uint32_t reserved;
    const uint8_t* active_rows;
    const uint8_t* nulls;
    const int64_t* partial_counts;
    uint8_t* const* row_bases;
} OmniSubOpCountGroupedMergeArgs;

OMNI_SUBOP_API int32_t omni_subop_count_abi_version(void) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_update_raw(
    const OmniSubOpCountArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_merge_partial(
    const OmniSubOpCountMergeArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_update_single_state(
    const OmniSubOpCountSingleStateArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_update_grouped(
    const OmniSubOpCountGroupedArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_merge_grouped(
    const OmniSubOpCountGroupedMergeArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* OMNI_SUBOP_COUNT_BATCH_H */
