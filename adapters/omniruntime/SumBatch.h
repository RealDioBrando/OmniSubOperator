#ifndef OMNI_SUBOP_ADAPTER_OMNIRUNTIME_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_OMNIRUNTIME_SUM_BATCH_H

#include <cstddef>
#include <cstdint>
#include <limits>

#include "kernel/sum_batch.h"

namespace omni::subop::omniruntime {

template <typename ValueAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumRawGrouped(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    return omni::subop::SumRawGrouped(
        numRows,
        valueAt,
        nullAt,
        stateValueAt,
        stateInitializedAt,
        markInitializedAt,
        result);
}

template <typename ValueAt, typename NullAt>
int32_t SumRawSingleGroup(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    int64_t* state,
    bool initialized,
    bool* stateInitialized,
    omni::subop::SumResult* result) {
    return omni::subop::SumRawSingleGroup(
        numRows,
        valueAt,
        nullAt,
        state,
        initialized,
        stateInitialized,
        result);
}

template <typename PartialAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumMergeGrouped(
    int32_t numRows,
    PartialAt partialAt,
    NullAt nullAt,
    StateValueAt stateValueAt,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    return omni::subop::SumMergeGrouped(
        numRows,
        partialAt,
        nullAt,
        stateValueAt,
        stateInitializedAt,
        markInitializedAt,
        result);
}

} // namespace omni::subop::omniruntime

#endif /* OMNI_SUBOP_ADAPTER_OMNIRUNTIME_SUM_BATCH_H */
