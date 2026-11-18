#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H

#include <vector>

#include "kernel/min_max_batch.h"

namespace omni::subop::omniruntime {

template <
    typename StatePtr,
    typename ValueAt,
    typename NullAt,
    typename LoadStateAt,
    typename StoreStateAt>
int32_t MinMaxRawGrouped(
    const std::vector<StatePtr>& rowStates,
    int32_t stateOffset,
    ValueAt valueAt,
    NullAt nullAt,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    omni::subop::MinMaxKind kind,
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
    return omni::subop::MinMaxStateGrouped(
        numRows,
        valueAt,
        nullAt,
        addressAt,
        loadStateAt,
        storeStateAt,
        kind,
        result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H */
