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
 * Count grouped rows through the engine-owned state-base API.
 *
 * rowStates already belongs to OmniOperator and is passed directly; this
 * adapter does not construct active-row, state-pointer, or (for COUNT(*))
 * NULL-mask vectors. A non-NULL nullAt mask is used for COUNT(expr) and for
 * COUNT(*) FILTER, because OmniOperator represents both as skip masks.
 */
template <typename NullAt>
int32_t CountRawBatch(
    const std::vector<uint8_t*>& rowStates,
    bool countAll,
    NullAt nullAt,
    uint32_t stateOffset,
    OmniSubOpCountResult* result) {
    if (rowStates.size() > std::numeric_limits<int32_t>::max()) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const auto numRows = static_cast<int32_t>(rowStates.size());
    if (countAll) {
        OmniSubOpCountGroupedArgs args{};
        args.abi_version = OMNI_SUBOP_ABI_VERSION;
        args.num_rows = numRows;
        args.mode = OMNI_SUBOP_COUNT_ALL;
        args.state_offset = stateOffset;
        args.active_rows = nullptr;
        args.nulls = nullptr;
        args.row_bases = reinterpret_cast<uint8_t* const*>(rowStates.data());
        return omni_subop_count_update_grouped(&args, result);
    }

    std::vector<uint8_t*> compactBases;
    compactBases.reserve(static_cast<std::size_t>(numRows));
    for (int32_t row = 0; row < numRows; ++row) {
        if (!nullAt(static_cast<std::size_t>(row))) {
            compactBases.push_back(rowStates[static_cast<std::size_t>(row)]);
        }
    }

    OmniSubOpCountGroupedArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(compactBases.size());
    args.mode = OMNI_SUBOP_COUNT_ALL;
    args.state_offset = stateOffset;
    args.active_rows = nullptr;
    args.nulls = nullptr;
    args.row_bases = compactBases.empty() ? nullptr : compactBases.data();
    return omni_subop_count_update_grouped(&args, result);
}

template <typename PartialAt, typename NullAt>
int32_t CountMergeBatch(
    const std::vector<uint8_t*>& rowStates,
    NullAt nullAt,
    PartialAt partialAt,
    uint32_t stateOffset,
    OmniSubOpCountResult* result) {
    if (rowStates.size() > std::numeric_limits<int32_t>::max()) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const auto numRows = static_cast<int32_t>(rowStates.size());
    bool hasNull = false;
    for (int32_t row = 0; row < numRows; ++row) {
        if (nullAt(static_cast<std::size_t>(row))) {
            hasNull = true;
            break;
        }
    }

    std::vector<uint8_t*> compactBases;
    std::vector<int64_t> compactPartial;
    if (hasNull) {
        compactBases.reserve(static_cast<std::size_t>(numRows));
        compactPartial.reserve(static_cast<std::size_t>(numRows));
        for (int32_t row = 0; row < numRows; ++row) {
            const auto index = static_cast<std::size_t>(row);
            if (!nullAt(index)) {
                compactBases.push_back(rowStates[index]);
                compactPartial.push_back(partialAt(index));
            }
        }
    } else {
        compactPartial.resize(static_cast<std::size_t>(numRows));
        for (int32_t row = 0; row < numRows; ++row) {
            compactPartial[static_cast<std::size_t>(row)] = partialAt(static_cast<std::size_t>(row));
        }
    }

    OmniSubOpCountGroupedMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(compactPartial.size());
    args.state_offset = stateOffset;
    args.active_rows = nullptr;
    args.nulls = nullptr;
    args.partial_counts = compactPartial.empty() ? nullptr : compactPartial.data();
    args.row_bases = hasNull
        ? (compactBases.empty() ? nullptr : compactBases.data())
        : reinterpret_cast<uint8_t* const*>(rowStates.data());
    return omni_subop_count_merge_grouped(&args, result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_COUNT_BATCH_H */
