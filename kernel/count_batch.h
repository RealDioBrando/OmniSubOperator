/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Narrow C ABI for the Phase 0 count micro-operator.
 *
 * The kernel operates on normalized row masks and per-row engine-state
 * handles. The engine adapter owns vector decoding, group lookup, and the
 * accumulator memory; the kernel never sees engine types.
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

OMNI_SUBOP_API int32_t omni_subop_count_abi_version(void) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_update_raw(
    const OmniSubOpCountArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

OMNI_SUBOP_API int32_t omni_subop_count_merge_partial(
    const OmniSubOpCountMergeArgs* args,
    OmniSubOpCountResult* result) OMNI_SUBOP_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#endif /* OMNI_SUBOP_COUNT_BATCH_H */
