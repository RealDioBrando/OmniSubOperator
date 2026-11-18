#ifndef OMNI_SUBOP_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_DECIMAL_SUM_BATCH_H

#include <cstdint>
#include <limits>

#include "kernel/sum_batch.h"

namespace omni::subop {

#if defined(__SIZEOF_INT128__)
__extension__ using DecimalInt128 = __int128;
__extension__ using DecimalUInt128 = unsigned __int128;
#else
struct DecimalInt128 {
    uint64_t low = 0;
    int64_t high = 0;
};
#endif

enum DecimalSumOverflowMode {
    DECIMAL_SUM_WRAP_TRACK = 0,
    DECIMAL_SUM_STRICT_NULL = 1,
};

/*
 * Engine-neutral decimal SUM state.
 *
 * sum and overflow mirror the Spark/Velox wrapping accumulator. count is the
 * number of non-NULL inputs represented by this state; invalid marks an engine
 * policy that poisons the state on the first arithmetic overflow.
 */
struct DecimalSumState {
    DecimalInt128 sum = 0;
    int64_t overflow = 0;
    int64_t count = 0;
    bool invalid = false;
};

namespace decimal_sum_detail {

inline bool IsInt128Available() {
#if defined(__SIZEOF_INT128__)
    return true;
#else
    return false;
#endif
}

#if defined(__SIZEOF_INT128__)
inline int64_t AddWithOverflow(
    DecimalInt128 lhs,
    DecimalInt128 rhs,
    DecimalInt128* result) {
    const bool lhsNegative = lhs < 0;
    const bool rhsNegative = rhs < 0;
    if (lhsNegative != rhsNegative) {
        *result = lhs + rhs;
        return 0;
    }

    const DecimalUInt128 unsignedSum =
        static_cast<DecimalUInt128>(lhs) +
        static_cast<DecimalUInt128>(rhs);
    const DecimalUInt128 overflowBit =
        static_cast<DecimalUInt128>(1) << 127;
    DecimalInt128 wrapped = static_cast<DecimalInt128>(unsignedSum & ~overflowBit);
    if (lhsNegative) {
        wrapped = -wrapped;
    }
    *result = wrapped;
    return lhsNegative ? -static_cast<int64_t>(unsignedSum >> 127)
                       : static_cast<int64_t>(unsignedSum >> 127);
}

inline bool AdjustForOverflow(
    DecimalInt128 sum,
    int64_t overflow,
    DecimalInt128* result) {
    if ((overflow == 1 && sum < 0) || (overflow == -1 && sum > 0)) {
        const DecimalUInt128 multiplier =
            static_cast<DecimalUInt128>(1) << 127;
        *result = static_cast<DecimalInt128>(
            multiplier * overflow + sum);
        return true;
    }
    if (overflow != 0) {
        return false;
    }
    *result = sum;
    return true;
}
#else
inline int64_t AddWithOverflow(
    DecimalInt128,
    DecimalInt128,
    DecimalInt128*) {
    return 0;
}

inline bool AdjustForOverflow(
    DecimalInt128,
    int64_t,
    DecimalInt128*) {
    return false;
}
#endif

inline bool ApplyInput(
    DecimalSumState& state,
    DecimalInt128 value,
    DecimalSumOverflowMode mode) {
    if (state.invalid) {
        return false;
    }
    if (state.count == 0) {
        state.sum = value;
        state.overflow = 0;
        state.count = 1;
        return true;
    }

    DecimalInt128 result = 0;
    const int64_t delta = AddWithOverflow(state.sum, value, &result);
    if (mode == DECIMAL_SUM_STRICT_NULL && delta != 0) {
        state.invalid = true;
        state.count = -1;
        state.overflow = 0;
        state.sum = 0;
        return false;
    }

    state.sum = result;
    state.overflow += delta;
    if (state.count != std::numeric_limits<int64_t>::max()) {
        ++state.count;
    }
    return true;
}

} // namespace decimal_sum_detail

/*
 * Grouped raw decimal SUM update. The adapter supplies row value/NULL callbacks,
 * a stable state address for validation, and engine-specific load/store
 * callbacks. All state addresses are validated before the first store.
 *
 * A duplicated state address is processed sequentially, preserving duplicate
 * group semantics without a group map.
 */
template <typename ValueAt, typename NullAt, typename StateAddressAt, typename LoadStateAt, typename StoreStateAt>
int32_t DecimalSumRawGrouped(
    int32_t numRows,
    ValueAt valueAt,
    NullAt nullAt,
    StateAddressAt stateAddressAt,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode mode,
    SumResult* result) {
    if (numRows < 0 ||
        (mode != DECIMAL_SUM_WRAP_TRACK && mode != DECIMAL_SUM_STRICT_NULL)) {
        return SUM_INVALID_ARGUMENT;
    }
    if (!decimal_sum_detail::IsInt128Available()) {
        return SUM_UNSUPPORTED;
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
        DecimalSumState state = loadStateAt(row);
        const DecimalInt128 value = static_cast<DecimalInt128>(valueAt(row));
        decimal_sum_detail::ApplyInput(state, value, mode);
        storeStateAt(row, state);
    }

    if (stats.counted_rows != 0 && allSameState) {
        stats.groups_touched = 1;
    }
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

inline bool DecimalSumAdjustForOverflow(
    DecimalInt128 sum,
    int64_t overflow,
    DecimalInt128* result) {
    return decimal_sum_detail::AdjustForOverflow(sum, overflow, result);
}

} // namespace omni::subop

#endif /* OMNI_SUBOP_DECIMAL_SUM_BATCH_H */