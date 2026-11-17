#ifndef OMNI_SUBOP_SUM_BATCH_H
#define OMNI_SUBOP_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <unordered_map>

namespace omni::subop {

enum SumStatus {
    SUM_OK = 0,
    SUM_INVALID_ARGUMENT = -1,
    SUM_UNSUPPORTED = -2,
    SUM_INVALID_STATE = -3,
    SUM_OVERFLOW = -4,
    SUM_INTERNAL_ERROR = -5,
};

struct SumResult {
    uint64_t selected_rows = 0;
    uint64_t counted_rows = 0;
    uint64_t null_skipped_rows = 0;
    uint32_t groups_touched = 0;
};

namespace sum_detail {

inline int64_t WrapAdd(int64_t left, int64_t right) {
    return static_cast<int64_t>(
        static_cast<uint64_t>(left) + static_cast<uint64_t>(right));
}

struct GroupDelta {
    bool present = false;
    bool initialized = false;
    int64_t delta = 0;
};

inline void StoreResult(const SumResult& stats, SumResult* result) {
    if (result != nullptr) {
        *result = stats;
    }
}

inline int32_t ValidateRowCount(int32_t numRows) {
    return numRows < 0 ? SUM_INVALID_ARGUMENT : SUM_OK;
}

} // namespace sum_detail

/*
 * Single-group raw update. The engine supplies the decoded value and NULL
 * predicate. The state is updated only after the complete row range has been
 * inspected.
 */
template <typename ValueAt, typename NullAt>
int32_t SumRawSingleGroup(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    int64_t* state,
    bool initialized,
    bool* stateInitialized,
    SumResult* result) {
    const int32_t rowStatus = sum_detail::ValidateRowCount(numRows);
    if (rowStatus != SUM_OK) {
        return rowStatus;
    }
    if (state == nullptr && numRows > 0) {
        return SUM_INVALID_STATE;
    }

    bool hasValue = false;
    int64_t delta = 0;
    SumResult stats{};
    for (int32_t row = 0; row < numRows; ++row) {
        ++stats.selected_rows;
        if (nullAt(row)) {
            ++stats.null_skipped_rows;
            continue;
        }
        ++stats.counted_rows;
        hasValue = true;
        delta = sum_detail::WrapAdd(delta, valueAt(row));
    }

    if (hasValue) {
        *state = initialized ? sum_detail::WrapAdd(*state, delta) : delta;
        if (stateInitialized != nullptr) {
            *stateInitialized = true;
        }
        stats.groups_touched = 1;
    }
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

/*
 * Grouped raw update. StateValueAt(row) returns the address of the engine's
 * int64 accumulator. StateInitializedAt(row) tells whether that accumulator
 * already contains a value. MarkInitializedAt(row) is invoked only after the
 * complete validation and all state writes have succeeded.
 */
template <typename ValueAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumRawGrouped(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumResult* result) {
    const int32_t rowStatus = sum_detail::ValidateRowCount(numRows);
    if (rowStatus != SUM_OK) {
        return rowStatus;
    }

    std::unordered_map<int64_t*, sum_detail::GroupDelta> deltas;
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
        auto& entry = deltas[state];
        if (!entry.present) {
            entry.present = true;
            entry.initialized = stateInitializedAt(row);
        }
        entry.delta = sum_detail::WrapAdd(entry.delta, valueAt(row));
        ++stats.counted_rows;
    }

    for (auto& entry : deltas) {
        *entry.first = entry.second.initialized
            ? sum_detail::WrapAdd(*entry.first, entry.second.delta)
            : entry.second.delta;
    }
    for (int32_t row = 0; row < numRows; ++row) {
        if (!nullAt(row)) {
            markInitializedAt(row);
        }
    }
    stats.groups_touched = static_cast<uint32_t>(deltas.size());
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

/*
 * Grouped partial-result merge. Values are already normalized int64 partial
 * sums. NULL partial rows are ignored and do not create a state.
 */
template <typename PartialAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumMergeGrouped(
    int32_t numRows,
    PartialAt partialAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumResult* result) {
    return SumRawGrouped(
        numRows,
        partialAt,
        nullAt,
        stateValueAt,
        stateInitializedAt,
        markInitializedAt,
        result);
}

} // namespace omni::subop

#endif /* OMNI_SUBOP_SUM_BATCH_H */
