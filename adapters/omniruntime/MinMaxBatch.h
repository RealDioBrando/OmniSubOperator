#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H

#include <cstdint>
#include <vector>

#include "kernel/min_max_batch.h"

namespace omni::subop::omniruntime {

/*
 * OmniOperator grouped min/max adapter.
 *
 * OmniOperator already supplies one state pointer per selected row. This
 * adapter materializes the row-aligned value, state, and NULL arrays once,
 * then uses the same packed kernel contract as the Velox adapter. State
 * updates remain sequential, so duplicate group pointers observe the previous
 * row's result.
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
    if (stateOffset < 0) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto numRows = static_cast<int32_t>(rowStates.size());
    const auto wordCount = (static_cast<size_t>(numRows) + 63) / 64;
    std::vector<int64_t> values(static_cast<size_t>(numRows), 0);
    std::vector<int64_t*> stateValues(static_cast<size_t>(numRows), nullptr);
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
            stateValues[static_cast<size_t>(row)] = reinterpret_cast<int64_t*>(
                static_cast<uint8_t*>(
                    static_cast<void*>(rowStates[static_cast<size_t>(row)])) +
                stateOffset);
        }
        values[static_cast<size_t>(row)] = valueAt(row);
    }

    omni::subop::Int64BatchView view;
    view.num_rows = numRows;
    view.row_begin = 0;
    view.row_end = numRows;
    view.value_count = numRows;
    view.values = values.empty() ? nullptr : values.data();
    view.state_values = stateValues.empty() ? nullptr : stateValues.data();
    view.null_mask = nullWords.empty() ? nullptr : nullWords.data();
    view.trusted = false;

    auto initializedAt = [&](int32_t row) {
        return loadStateAt(row).initialized;
    };
    auto markInitializedAt = [&](int32_t row) {
        auto state = loadStateAt(row);
        state.value = *stateValues[static_cast<size_t>(row)];
        state.initialized = true;
        storeStateAt(row, state);
    };
    return omni::subop::MinMaxRawPacked(
        view,
        initializedAt,
        markInitializedAt,
        kind,
        omni::subop::MIN_MAX_AUTO,
        result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_MIN_MAX_BATCH_H */