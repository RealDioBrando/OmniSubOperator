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

    SumResult stats{};
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        if (stateValueAt(row) == nullptr) {
            return SUM_INVALID_STATE;
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
    stats.groups_touched = stats.counted_rows != 0 ? 1 : 0;
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

    SumResult stats{};
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        if (stateAddressAt(row) == nullptr) {
            return SUM_INVALID_STATE;
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
    stats.groups_touched = stats.counted_rows != 0 ? 1 : 0;
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
