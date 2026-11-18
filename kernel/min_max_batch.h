#ifndef OMNI_SUBOP_MIN_MAX_BATCH_H
#define OMNI_SUBOP_MIN_MAX_BATCH_H

#include <cstdint>

#include "kernel/sum_batch.h"

namespace omni::subop {

enum MinMaxKind {
    MIN_MAX_MIN = 0,
    MIN_MAX_MAX = 1,
};

struct MinMaxState {
    int64_t value = 0;
    bool initialized = false;
};

/*
 * Direct grouped min/max update. State pointers are validated before the first
 * engine write. Duplicate state pointers are updated sequentially, so a
 * repeated group in the same batch observes the prior update.
 */
template <typename ValueAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t MinMaxRawGroupedDirect(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    MinMaxKind kind,
    SumResult* result) {
    if (numRows < 0 || (kind != MIN_MAX_MIN && kind != MIN_MAX_MAX)) {
        return SUM_INVALID_ARGUMENT;
    }

    bool firstState = true;
    bool allSameState = true;
    int64_t* firstStateValue = nullptr;
    SumResult stats{};
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        int64_t* state = stateValueAt(row);
        if (state == nullptr) {
            return SUM_INVALID_STATE;
        }
        if (firstState) {
            firstState = false;
            firstStateValue = state;
        } else if (state != firstStateValue) {
            allSameState = false;
        }
        ++stats.counted_rows;
    }

    for (int32_t row = 0; row < numRows; ++row) {
        if (nullAt(row)) {
            continue;
        }
        int64_t* state = stateValueAt(row);
        const int64_t value = valueAt(row);
        if (!stateInitializedAt(row)) {
            *state = value;
        } else if (kind == MIN_MAX_MIN) {
            if (value < *state) {
                *state = value;
            }
        } else {
            if (value > *state) {
                *state = value;
            }
        }
        markInitializedAt(row);
    }
    stats.groups_touched =
        stats.counted_rows != 0 && allSameState ? 1 : 0;
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

template <typename ValueAt, typename NullAt, typename StateAddressAt, typename LoadStateAt, typename StoreStateAt>
int32_t MinMaxStateGrouped(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    StateAddressAt stateAddressAt,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    MinMaxKind kind,
    SumResult* result) {
    if (numRows < 0 || (kind != MIN_MAX_MIN && kind != MIN_MAX_MAX)) {
        return SUM_INVALID_ARGUMENT;
    }

    bool firstState = true;
    bool allSameState = true;
    const void* firstAddress = nullptr;
    SumResult stats{};
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        const void* address = stateAddressAt(row);
        if (address == nullptr) {
            return SUM_INVALID_STATE;
        }
        if (firstState) {
            firstState = false;
            firstAddress = address;
        } else if (address != firstAddress) {
            allSameState = false;
        }
        ++stats.counted_rows;
    }

    for (int32_t row = 0; row < numRows; ++row) {
        if (nullAt(row)) {
            continue;
        }
        MinMaxState state = loadStateAt(row);
        const int64_t value = valueAt(row);
        if (!state.initialized) {
            state.value = value;
            state.initialized = true;
        } else if (kind == MIN_MAX_MIN) {
            if (value < state.value) {
                state.value = value;
            }
        } else if (value > state.value) {
            state.value = value;
        }
        storeStateAt(row, state);
    }
    stats.groups_touched =
        stats.counted_rows != 0 && allSameState ? 1 : 0;
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

template <typename ValueAt, typename NullAt>
int32_t MinMaxRawSingleGroup(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    int64_t* state,
    bool initialized,
    bool* stateInitialized,
    MinMaxKind kind,
    SumResult* result) {
    if (numRows < 0 || (kind != MIN_MAX_MIN && kind != MIN_MAX_MAX)) {
        return SUM_INVALID_ARGUMENT;
    }
    if (numRows > 0 && state == nullptr) {
        return SUM_INVALID_STATE;
    }

    SumResult stats{};
    bool hasValue = initialized;
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        ++stats.counted_rows;
        const int64_t value = valueAt(row);
        if (!hasValue) {
            *state = value;
            hasValue = true;
        } else if (kind == MIN_MAX_MIN) {
            if (value < *state) {
                *state = value;
            }
        } else {
            if (value > *state) {
                *state = value;
            }
        }
    }
    if (hasValue && !initialized) {
        if (stateInitialized != nullptr) {
            *stateInitialized = true;
        }
    }
    stats.groups_touched = hasValue ? 1 : 0;
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

enum MinMaxExecutionMode {
    MIN_MAX_AUTO = 0,
    MIN_MAX_SCALAR = 1,
    MIN_MAX_SVE = 2,
};

namespace min_max_detail {

struct MinMaxPackedScan {
    SumResult stats{};
    bool firstState = true;
    bool allSameState = true;
    int64_t* firstStateValue = nullptr;
};

inline int32_t ScanPacked(const Int64BatchView& view, MinMaxPackedScan& scan) {
    const auto status = sum_detail::ValidateBatchView(view);
    if (status != SUM_OK) {
        return status;
    }
    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (!sum_detail::IsActiveAt(view.active_mask, row)) {
            continue;
        }
        ++scan.stats.selected_rows;
        if (sum_detail::IsNullAt(view.null_mask, row)) {
            ++scan.stats.null_skipped_rows;
            continue;
        }
        ++scan.stats.counted_rows;
        int64_t* state = view.state_values[row];
        if (scan.firstState) {
            scan.firstState = false;
            scan.firstStateValue = state;
        } else if (state != scan.firstStateValue) {
            scan.allSameState = false;
        }
    }
    if (scan.stats.counted_rows != 0 && scan.allSameState) {
        scan.stats.groups_touched = 1;
    }
    return SUM_OK;
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void UpdatePackedRow(
    const Int64BatchView& view,
    int32_t row,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    int64_t* state = view.state_values[row];
    const int64_t value = sum_detail::LoadValue(view, row);
    if (!stateInitializedAt(row)) {
        *state = value;
    } else if (kind == MIN_MAX_MIN) {
        if (value < *state) {
            *state = value;
        }
    } else if (value > *state) {
        *state = value;
    }
    markInitializedAt(row);
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void WalkScalar(
    const Int64BatchView& view,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    if (view.value_layout == SUM_VALUE_FLAT && view.active_mask == nullptr) {
        if (view.null_mask == nullptr) {
            int32_t row = view.row_begin;
            for (; row + 3 < view.row_end; row += 4) {
                UpdatePackedRow(view, row, kind, stateInitializedAt, markInitializedAt);
                UpdatePackedRow(view, row + 1, kind, stateInitializedAt, markInitializedAt);
                UpdatePackedRow(view, row + 2, kind, stateInitializedAt, markInitializedAt);
                UpdatePackedRow(view, row + 3, kind, stateInitializedAt, markInitializedAt);
            }
            for (; row < view.row_end; ++row) {
                UpdatePackedRow(view, row, kind, stateInitializedAt, markInitializedAt);
            }
            return;
        }
        for (int32_t row = view.row_begin; row < view.row_end; ++row) {
            if (!sum_detail::IsNullAt(view.null_mask, row)) {
                UpdatePackedRow(view, row, kind, stateInitializedAt, markInitializedAt);
            }
        }
        return;
    }
    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (sum_detail::IsActiveAt(view.active_mask, row) &&
            !sum_detail::IsNullAt(view.null_mask, row)) {
            UpdatePackedRow(view, row, kind, stateInitializedAt, markInitializedAt);
        }
    }
}

#if defined(__ARM_FEATURE_SVE)
template <typename StateInitializedAt, typename MarkInitializedAt>
inline void WalkSve(
    const Int64BatchView& view,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    if (view.active_mask == nullptr && view.null_mask == nullptr) {
        WalkScalar(view, kind, stateInitializedAt, markInitializedAt);
        return;
    }
    const int32_t firstWord = view.row_begin / 32;
    const int32_t lastWord = (view.row_end + 31) / 32;
    for (int32_t word = firstWord; word < lastWord; ++word) {
        const int32_t wordBegin = word * 32;
        uint32_t validBits = 0xFFFFFFFFu;
        if (view.active_mask != nullptr) {
            validBits &= reinterpret_cast<const uint32_t*>(view.active_mask)[word];
        }
        if (view.null_mask != nullptr) {
            validBits &= ~reinterpret_cast<const uint32_t*>(view.null_mask)[word];
        }
        const int32_t begin = std::max(wordBegin, view.row_begin) - wordBegin;
        const int32_t end = std::min(wordBegin + 32, view.row_end) - wordBegin;
        const uint64_t range = ((1ULL << end) - 1ULL) & ~((1ULL << begin) - 1ULL);
        validBits &= static_cast<uint32_t>(range);
        if (validBits == 0) {
            continue;
        }
        const svbool_t predicate = sum_detail::LoadPredicate32(&validBits);
        uint32_t bits = sum_detail::StorePredicate32(predicate);
        while (bits != 0) {
            const int32_t lane = __builtin_ctz(bits);
            UpdatePackedRow(
                view,
                wordBegin + lane,
                kind,
                stateInitializedAt,
                markInitializedAt);
            bits &= bits - 1;
        }
    }
}
#endif

template <typename StateInitializedAt, typename MarkInitializedAt>
inline int32_t UpdateStateBaseRow(
    const SumInt64StateBaseView& view,
    int32_t row,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    int64_t* state = sum_detail::StateBaseAt(view, row);
    if (state == nullptr) {
        return SUM_INVALID_STATE;
    }
    const int64_t value = sum_detail::LoadStateBaseValue(view, row);
    if (!stateInitializedAt(row)) {
        *state = value;
    } else if (kind == MIN_MAX_MIN) {
        if (value < *state) {
            *state = value;
        }
    } else if (value > *state) {
        *state = value;
    }
    markInitializedAt(row);
    return SUM_OK;
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline int32_t MinMaxStateBaseRows(
    const SumInt64StateBaseView& view,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumResult* result) {
    if (kind != MIN_MAX_MIN && kind != MIN_MAX_MAX) {
        return SUM_INVALID_ARGUMENT;
    }
    const auto status = sum_detail::ValidateStateBaseView(view);
    if (status != SUM_OK) {
        return status;
    }
    const auto baseStatus = sum_detail::ValidateStateBaseBases(view);
    if (baseStatus != SUM_OK) {
        return baseStatus;
    }

    SumResult stats{};
    const auto trackStats = result != nullptr;
    if (view.value_layout == SUM_VALUE_FLAT &&
        view.active_mask == nullptr && view.valid_mask == nullptr) {
        int32_t row = view.row_begin;
        for (; row + 3 < view.row_end; row += 4) {
            for (int32_t offset = 0; offset < 4; ++offset) {
                const auto updateStatus = UpdateStateBaseRow(
                    view,
                    row + offset,
                    kind,
                    stateInitializedAt,
                    markInitializedAt);
                if (updateStatus != SUM_OK) {
                    return updateStatus;
                }
            }
            if (trackStats) {
                stats.selected_rows += 4;
                stats.counted_rows += 4;
            }
        }
        for (; row < view.row_end; ++row) {
            const auto updateStatus = UpdateStateBaseRow(
                view,
                row,
                kind,
                stateInitializedAt,
                markInitializedAt);
            if (updateStatus != SUM_OK) {
                return updateStatus;
            }
            if (trackStats) {
                ++stats.selected_rows;
                ++stats.counted_rows;
            }
        }
    } else {
        for (int32_t row = view.row_begin; row < view.row_end; ++row) {
            if (!sum_detail::IsActiveAt(view.active_mask, row)) {
                continue;
            }
            if (trackStats) {
                ++stats.selected_rows;
            }
            if (!sum_detail::IsValidAt(view.valid_mask, row)) {
                if (trackStats) {
                    ++stats.null_skipped_rows;
                }
                continue;
            }
            const auto updateStatus = UpdateStateBaseRow(
                view,
                row,
                kind,
                stateInitializedAt,
                markInitializedAt);
            if (updateStatus != SUM_OK) {
                return updateStatus;
            }
            if (trackStats) {
                ++stats.counted_rows;
            }
        }
    }
    if (trackStats && stats.counted_rows != 0) {
        stats.groups_touched = 1;
    }
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}
} // namespace min_max_detail

/*
 * Direct min/max update over borrowed group bases and value bitmaps. No
 * row-aligned state-pointer or NULL-mask arrays are constructed. A trusted
 * view relies on the engine's valid-group-base contract; an untrusted view
 * prevalidates every active, non-NULL base before the first write.
 */
template <typename StateInitializedAt, typename MarkInitializedAt>
int32_t MinMaxStateBasePacked(
    const SumInt64StateBaseView& view,
    MinMaxKind kind,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumResult* result) {
    return min_max_detail::MinMaxStateBaseRows(
        view,
        kind,
        stateInitializedAt,
        markInitializedAt,
        result);
}

template <typename StateInitializedAt, typename MarkInitializedAt>
int32_t MinMaxRawPacked(
    const Int64BatchView& view,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    MinMaxKind kind,
    MinMaxExecutionMode mode,
    SumResult* result) {
    if ((kind != MIN_MAX_MIN && kind != MIN_MAX_MAX) ||
        (mode != MIN_MAX_AUTO && mode != MIN_MAX_SCALAR && mode != MIN_MAX_SVE)) {
        return SUM_INVALID_ARGUMENT;
    }
    min_max_detail::MinMaxPackedScan scan;
    if (!view.trusted || result != nullptr) {
        const auto status = min_max_detail::ScanPacked(view, scan);
        if (status != SUM_OK) {
            return status;
        }
    }
    MinMaxExecutionMode resolved = mode;
    if (mode == MIN_MAX_AUTO) {
        resolved = MIN_MAX_SCALAR;
#if defined(__ARM_FEATURE_SVE)
        if (view.active_mask != nullptr && view.null_mask == nullptr &&
            view.row_end - view.row_begin >= 32 &&
            sum_detail::CanUsePackedSve()) {
            resolved = MIN_MAX_SVE;
        }
#endif
    }
    if (resolved == MIN_MAX_SVE) {
#if defined(__ARM_FEATURE_SVE)
        if (!sum_detail::CanUsePackedSve()) {
            return SUM_UNSUPPORTED;
        }
        min_max_detail::WalkSve(view, kind, stateInitializedAt, markInitializedAt);
#else
        return SUM_UNSUPPORTED;
#endif
    } else {
        min_max_detail::WalkScalar(view, kind, stateInitializedAt, markInitializedAt);
    }
    sum_detail::StoreResult(scan.stats, result);
    return SUM_OK;
}

template <typename PartialAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t MinMaxMergeGrouped(
    int32_t numRows,
    PartialAt partialAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    MinMaxKind kind,
    SumResult* result) {
    return MinMaxRawGroupedDirect(
        numRows,
        partialAt,
        nullAt,
        stateValueAt,
        stateInitializedAt,
        markInitializedAt,
        kind,
        result);
}

} // namespace omni::subop

#endif /* OMNI_SUBOP_MIN_MAX_BATCH_H */
