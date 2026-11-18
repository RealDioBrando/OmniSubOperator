#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H

#include <cstdint>
#include <vector>

#include "kernel/decimal_sum_batch.h"

namespace omni::subop::omniruntime {

/*
 * Decimal raw-input adapter over OmniOperator's row-state pointer array.
 * Values and state addresses are materialized once, then the shared packed
 * walker applies the same row-order state update as the callback path.
 */
template <
    typename StatePtr,
    typename ValueAt,
    typename NullAt,
    typename LoadStateAt,
    typename StoreStateAt>
int32_t DecimalSumRawGrouped(
    const std::vector<StatePtr>& rowStates,
    int32_t stateOffset,
    ValueAt valueAt,
    NullAt nullAt,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    omni::subop::DecimalSumOverflowMode mode,
    omni::subop::SumResult* result) {
    if (stateOffset < 0) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto numRows = static_cast<int32_t>(rowStates.size());
    const auto wordCount = (static_cast<size_t>(numRows) + 63) / 64;
    std::vector<omni::subop::DecimalInt128> values(
        static_cast<size_t>(numRows), 0);
    std::vector<const void*> stateValues(static_cast<size_t>(numRows), nullptr);
    std::vector<uint64_t> nullWords;
    for (int32_t row = 0; row < numRows; ++row) {
        if (nullAt(row)) {
            if (nullWords.empty()) {
                nullWords.assign(wordCount, 0);
            }
            nullWords[static_cast<size_t>(row) / 64] |=
                1ULL << (static_cast<size_t>(row) % 64);
            continue;
        }
        if (rowStates[static_cast<size_t>(row)] != nullptr) {
            stateValues[static_cast<size_t>(row)] =
                static_cast<const void*>(
                    static_cast<const uint8_t*>(
                        static_cast<const void*>(rowStates[static_cast<size_t>(row)])) +
                    stateOffset);
        }
        values[static_cast<size_t>(row)] = valueAt(row);
    }

    omni::subop::DecimalSumBatchView view;
    view.num_rows = numRows;
    view.row_begin = 0;
    view.row_end = numRows;
    view.value_count = numRows;
    view.values = values.empty() ? nullptr : values.data();
    view.state_values = stateValues.empty() ? nullptr : stateValues.data();
    view.null_mask = nullWords.empty() ? nullptr : nullWords.data();
    view.trusted = false;
    return omni::subop::DecimalSumRawPacked(
        view,
        loadStateAt,
        storeStateAt,
        mode,
        omni::subop::SUM_EXECUTION_AUTO,
        result);
}

/*
 * Partial-result merge adapter. The engine normalizes its intermediate row to
 * DecimalSumPartial through PartialAt and supplies its state load/store
 * callbacks; the shared kernel owns merge ordering and overflow propagation.
 */
template <
    typename StatePtr,
    typename PartialAt,
    typename NullAt,
    typename LoadStateAt,
    typename StoreStateAt>
int32_t DecimalSumPartialGrouped(
    const std::vector<StatePtr>& rowStates,
    int32_t stateOffset,
    PartialAt partialAt,
    NullAt nullAt,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    omni::subop::DecimalSumOverflowMode mode,
    omni::subop::SumResult* result) {
    if (stateOffset < 0) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto numRows = static_cast<int32_t>(rowStates.size());
    auto addressAt = [&](int32_t row) -> const void* {
        if (rowStates[static_cast<size_t>(row)] == nullptr) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(
                   static_cast<const void*>(rowStates[static_cast<size_t>(row)])) +
            stateOffset;
    };
    return omni::subop::DecimalSumMergeGrouped(
        numRows,
        partialAt,
        nullAt,
        addressAt,
        loadStateAt,
        storeStateAt,
        mode,
        result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H */