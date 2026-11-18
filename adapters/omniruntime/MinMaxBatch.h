#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/min_max_batch.h"

namespace omni::subop::omniruntime {

/*
 * OmniOperator grouped min/max adapter.
 *
 * OmniOperator already supplies one state pointer per selected row. Keep that
 * engine-owned vector as the state view and process rows sequentially, rather
 * than materializing value, state-pointer, and NULL-mask arrays. State updates
 * remain sequential, so duplicate group pointers observe the previous row's
 * result. The shared kernel validates every selected address before the first
 * write.
 */
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