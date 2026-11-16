/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Phase 0 OmniOperator adapter for the count micro-operator.
 *
 * The caller supplies callbacks for the engine-owned state address, the
 * partial value, and the NULL mask. This keeps the adapter independent of the
 * concrete aggregator template and avoids changing the reference tree.
 */

#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_COUNT_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_COUNT_BATCH_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/count_batch.h"

namespace omni::subop::omniruntime {

namespace count_detail {

inline int32_t SubmitSingleState(
    int64_t* state,
    uint64_t selectedRows,
    uint64_t countedRows,
    uint64_t nullSkippedRows,
    int64_t delta,
    OmniSubOpCountResult* result) {
    OmniSubOpCountSingleStateArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.state = state;
    args.selected_rows = selectedRows;
    args.counted_rows = countedRows;
    args.null_skipped_rows = nullSkippedRows;
    args.delta = delta;
    return omni_subop_count_update_single_state(&args, result);
}

} // namespace count_detail

/*
 * Count a contiguous single-group batch without materializing row-state,
 * active-row, or NULL-mask vectors.
 */
template <typename NullAt>
int32_t CountRawSingleGroup(
    int32_t numRows,
    int64_t* state,
    bool countAll,
    NullAt nullAt,
    OmniSubOpCountResult* result) {
    if (numRows < 0) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const uint64_t selectedRows = static_cast<uint64_t>(numRows);
    uint64_t nullSkippedRows = 0;
    if (!countAll) {
        for (int32_t row = 0; row < numRows; ++row) {
            if (nullAt(static_cast<std::size_t>(row))) {
                ++nullSkippedRows;
            }
        }
    }
    const uint64_t countedRows = selectedRows - nullSkippedRows;
    return count_detail::SubmitSingleState(
        state,
        selectedRows,
        countedRows,
        nullSkippedRows,
        static_cast<int64_t>(countedRows),
        result);
}

/*
 * Merge contiguous single-group partial counts without normalized vectors.
 * The adapter validates each partial value and sums it before calling the
 * kernel, so the engine state is untouched when a later row is invalid.
 */
template <typename PartialAt, typename NullAt>
int32_t CountMergeSingleGroup(
    int32_t numRows,
    int64_t* state,
    NullAt nullAt,
    PartialAt partialAt,
    OmniSubOpCountResult* result) {
    if (numRows < 0) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const uint64_t selectedRows = static_cast<uint64_t>(numRows);
    uint64_t countedRows = 0;
    uint64_t nullSkippedRows = 0;
    int64_t delta = 0;
    int32_t failure = OMNI_SUBOP_COUNT_OK;

    for (int32_t row = 0; row < numRows; ++row) {
        const auto index = static_cast<std::size_t>(row);
        if (nullAt(index)) {
            ++nullSkippedRows;
            continue;
        }
        const int64_t value = partialAt(index);
        if (value < 0) {
            failure = OMNI_SUBOP_COUNT_INVALID_STATE;
            break;
        }
        ++countedRows;
        if (delta > std::numeric_limits<int64_t>::max() - value) {
            failure = OMNI_SUBOP_COUNT_OVERFLOW;
            break;
        }
        delta += value;
    }

    if (failure != OMNI_SUBOP_COUNT_OK) {
        return failure;
    }
    return count_detail::SubmitSingleState(
        state,
        selectedRows,
        countedRows,
        nullSkippedRows,
        delta,
        result);
}

/*
 * Count already-selected rowStates.
 *
 * rowStates contains one group-state pointer per input row. A typical caller
 * resolves the count state with:
 *
 *   [&](std::size_t row) {
 *       auto* state = CountState::CastState(
 *           rowStates[row] + aggStateOffset);
 *       return &state->count;
 *   }
 *
 * nullAt is only used for COUNT(expr); pass a lambda returning false for
 * COUNT(*).
 */
template <typename StateAt, typename NullAt>
int32_t CountRawBatch(
    const std::vector<uint8_t*>& rowStates,
    bool countAll,
    NullAt nullAt,
    StateAt stateAt,
    OmniSubOpCountResult* result) {
    if (rowStates.size() > std::numeric_limits<int32_t>::max()) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const auto numRows = static_cast<int32_t>(rowStates.size());
    std::vector<uint8_t> active(static_cast<std::size_t>(numRows), 1);
    std::vector<uint8_t> nulls;
    std::vector<int64_t*> rowCounts(static_cast<std::size_t>(numRows), nullptr);

    if (!countAll) {
        nulls.resize(static_cast<std::size_t>(numRows), 0);
    }
    for (int32_t row = 0; row < numRows; ++row) {
        if (!countAll && nullAt(static_cast<std::size_t>(row))) {
            nulls[static_cast<std::size_t>(row)] = 1;
        }
        rowCounts[static_cast<std::size_t>(row)] = stateAt(static_cast<std::size_t>(row));
    }

    OmniSubOpCountArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = numRows;
    args.mode = countAll ? OMNI_SUBOP_COUNT_ALL : OMNI_SUBOP_COUNT_EXPR;
    args.encoding = OMNI_SUBOP_VALUE_FLAT;
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls.empty() ? nullptr : nulls.data();
    args.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    return omni_subop_count_update_raw(&args, result);
}

/*
 * Merge already-selected partial count rows.
 *
 * partialAt(row) supplies the normalized BIGINT value. nullAt(row) identifies
 * a NULL partial value; the kernel skips that row rather than adding zero.
 */
template <typename StateAt, typename PartialAt, typename NullAt>
int32_t CountMergeBatch(
    const std::vector<uint8_t*>& rowStates,
    NullAt nullAt,
    PartialAt partialAt,
    StateAt stateAt,
    OmniSubOpCountResult* result) {
    if (rowStates.size() > std::numeric_limits<int32_t>::max()) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const auto numRows = static_cast<int32_t>(rowStates.size());
    std::vector<uint8_t> active(static_cast<std::size_t>(numRows), 1);
    std::vector<uint8_t> nulls(static_cast<std::size_t>(numRows), 0);
    std::vector<int64_t> partial(static_cast<std::size_t>(numRows), 0);
    std::vector<int64_t*> rowCounts(static_cast<std::size_t>(numRows), nullptr);

    for (int32_t row = 0; row < numRows; ++row) {
        const std::size_t index = static_cast<std::size_t>(row);
        if (nullAt(index)) {
            nulls[index] = 1;
        } else {
            partial[index] = partialAt(index);
        }
        rowCounts[index] = stateAt(index);
    }

    OmniSubOpCountMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = numRows;
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls.empty() ? nullptr : nulls.data();
    args.partial_counts = partial.empty() ? nullptr : partial.data();
    args.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    return omni_subop_count_merge_partial(&args, result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_COUNT_BATCH_H */
