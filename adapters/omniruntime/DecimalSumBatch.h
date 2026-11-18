#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/decimal_sum_batch.h"

namespace omni::subop::omniruntime {

/*
 * Decimal raw-input adapter over OmniOperator's row-state pointer array.
 * The engine's selected-row vector is already the row-aligned state view, so
 * the adapter hands it to the shared callback kernel directly. No value,
 * state-pointer, or NULL-mask arrays are materialized.
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
    if (stateOffset < 0 ||
        rowStates.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto numRows = static_cast<int32_t>(rowStates.size());
    auto addressAt = [&](int32_t row) -> const void* {
        auto* base = rowStates[static_cast<size_t>(row)];
        if (base == nullptr) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(
                   static_cast<const void*>(base)) +
            stateOffset;
    };
    return omni::subop::DecimalSumRawGrouped(
        numRows,
        valueAt,
        nullAt,
        addressAt,
        loadStateAt,
        storeStateAt,
        mode,
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
    if (stateOffset < 0 ||
        rowStates.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto numRows = static_cast<int32_t>(rowStates.size());
    auto addressAt = [&](int32_t row) -> const void* {
        auto* base = rowStates[static_cast<size_t>(row)];
        if (base == nullptr) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(
                   static_cast<const void*>(base)) +
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