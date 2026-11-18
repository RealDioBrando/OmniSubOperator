#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H

#include <vector>

#include "kernel/decimal_sum_batch.h"

namespace omni::subop::omniruntime {

/*
 * Decimal raw-input adapter over OmniOperator's row-state pointer array.
 * The engine code supplies value, NULL, state-load, and state-store callbacks;
 * this wrapper only maps row-state addresses into the shared kernel.
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
    const auto numRows = static_cast<int32_t>(rowStates.size());
    auto addressAt = [&](int32_t row) -> const void* {
        if (rowStates[static_cast<size_t>(row)] == nullptr) {
            return nullptr;
        }
        return static_cast<const uint8_t*>(
                   static_cast<const void*>(rowStates[static_cast<size_t>(row)])) +
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

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_DECIMAL_SUM_BATCH_H */
