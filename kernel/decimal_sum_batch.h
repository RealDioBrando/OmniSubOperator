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

/*
 * Engine-neutral decimal partial result used by intermediate merges.
 *
 * count == 0 means empty, count < 0 means the producer observed overflow, and
 * count > 0 is the number of input values represented by sum. The count is
 * advisory for engines that do not carry a source count; a value of one is
 * sufficient for merge semantics.
 */
struct DecimalSumPartial {
    DecimalInt128 sum = 0;
    int64_t count = 0;
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

    const DecimalUInt128 overflowBit =
        static_cast<DecimalUInt128>(1) << 127;
    DecimalUInt128 unsignedSum = 0;
    if (lhsNegative) {
        unsignedSum = static_cast<DecimalUInt128>(-lhs) +
            static_cast<DecimalUInt128>(-rhs);
    } else {
        unsignedSum = static_cast<DecimalUInt128>(lhs) +
            static_cast<DecimalUInt128>(rhs);
    }

    const DecimalInt128 magnitude =
        static_cast<DecimalInt128>(unsignedSum & ~overflowBit);
    *result = lhsNegative ? -magnitude : magnitude;
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

inline void ApplyPartial(
    DecimalSumState& state,
    const DecimalSumPartial& partial,
    DecimalSumOverflowMode mode) {
    if (partial.count == 0) {
        return;
    }
    if (partial.count < 0) {
        state.invalid = true;
        state.count = -1;
        state.overflow = 0;
        state.sum = 0;
        return;
    }
    if (state.invalid) {
        return;
    }
    if (state.count == 0) {
        state.sum = partial.sum;
        state.overflow = 0;
        state.count = partial.count;
        return;
    }

    DecimalInt128 result = 0;
    const int64_t delta = AddWithOverflow(state.sum, partial.sum, &result);
    if (mode == DECIMAL_SUM_STRICT_NULL && delta != 0) {
        state.invalid = true;
        state.count = -1;
        state.overflow = 0;
        state.sum = 0;
        return;
    }
    state.sum = result;
    state.overflow += delta;
    if (partial.count > 0 &&
        state.count <= std::numeric_limits<int64_t>::max() - partial.count) {
        state.count += partial.count;
    } else {
        state.count = std::numeric_limits<int64_t>::max();
    }
}

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
 * Engine-neutral row view for a packed decimal SUM batch.
 *
 * Values are materialized as 128-bit unscaled integers. state_values contains
 * an engine-owned address for each row; validation rejects a missing address
 * before any state is stored. active_mask and null_mask follow the same
 * 64-bit-word convention as the int64 SUM view. The view is intentionally
 * callback-based at the state boundary because decimal accumulator layouts
 * differ between Velox and OmniOperator.
 */
struct DecimalSumBatchView {
    int32_t num_rows = 0;
    int32_t row_begin = 0;
    int32_t row_end = 0;
    int32_t value_count = 0;
    int32_t index_count = 0;
    SumValueLayout value_layout = SUM_VALUE_FLAT;
    const DecimalInt128* values = nullptr;
    const int32_t* value_indices = nullptr;
    const void* const* state_values = nullptr;
    const uint64_t* active_mask = nullptr;
    const uint64_t* null_mask = nullptr;
    bool trusted = false;
};

namespace decimal_sum_detail {

inline DecimalInt128 LoadValue(
    const DecimalSumBatchView& view,
    int32_t row) {
    if (view.value_layout == SUM_VALUE_CONSTANT) {
        return view.values[0];
    }
    if (view.value_layout == SUM_VALUE_DICTIONARY) {
        return view.values[view.value_indices[row]];
    }
    return view.values[row];
}

inline int32_t ValidateBatchView(const DecimalSumBatchView& view) {
    if (view.num_rows < 0 || view.row_begin < 0 ||
        view.row_end < view.row_begin || view.row_end > view.num_rows) {
        return SUM_INVALID_ARGUMENT;
    }
    if (view.row_begin == view.row_end) {
        return SUM_OK;
    }
    if (view.state_values == nullptr || view.values == nullptr) {
        return SUM_INVALID_ARGUMENT;
    }
    if (view.value_layout == SUM_VALUE_FLAT) {
        if (view.value_count < view.row_end) {
            return SUM_INVALID_ARGUMENT;
        }
    } else if (view.value_layout == SUM_VALUE_CONSTANT) {
        if (view.value_count < 1) {
            return SUM_INVALID_ARGUMENT;
        }
    } else if (view.value_layout == SUM_VALUE_DICTIONARY) {
        if (view.value_indices == nullptr || view.value_count < 1 ||
            view.index_count < view.row_end) {
            return SUM_INVALID_ARGUMENT;
        }
    } else {
        return SUM_INVALID_ARGUMENT;
    }

    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (!sum_detail::IsActiveAt(view.active_mask, row) ||
            sum_detail::IsNullAt(view.null_mask, row)) {
            continue;
        }
        if (view.state_values[row] == nullptr) {
            return SUM_INVALID_STATE;
        }
        if (view.value_layout == SUM_VALUE_DICTIONARY) {
            const int32_t index = view.value_indices[row];
            if (index < 0 || index >= view.value_count) {
                return SUM_INVALID_STATE;
            }
        }
    }
    return SUM_OK;
}

struct DecimalBatchScan {
    SumResult stats{};
    bool firstState = true;
    bool allSameState = true;
    const void* firstStateValue = nullptr;
};

inline int32_t ScanPacked(
    const DecimalSumBatchView& view,
    DecimalBatchScan& scan) {
    const auto status = ValidateBatchView(view);
    if (status != SUM_OK) {
        return status;
    }
    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (!sum_detail::IsActiveAt(view.active_mask, row)) {
            continue;
        }
        ++scan.stats.selected_rows;
        if (sum_detail::IsNullAt(view.null_mask, row)) {
            ++scan.stats.null_skipped_rows;
            continue;
        }
        ++scan.stats.counted_rows;
        const void* state = view.state_values[row];
        if (scan.firstState) {
            scan.firstState = false;
            scan.firstStateValue = state;
        } else if (state != scan.firstStateValue) {
            scan.allSameState = false;
        }
    }
    if (scan.stats.counted_rows != 0 && scan.allSameState) {
        scan.stats.groups_touched = 1;
    }
    return SUM_OK;
}

template <typename LoadStateAt, typename StoreStateAt>
inline void UpdatePackedRow(
    const DecimalSumBatchView& view,
    int32_t row,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode mode) {
    DecimalSumState state = loadStateAt(row);
    decimal_sum_detail::ApplyInput(state, LoadValue(view, row), mode);
    storeStateAt(row, state);
}

template <typename LoadStateAt, typename StoreStateAt>
inline void WalkScalar(
    const DecimalSumBatchView& view,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode mode) {
    if (view.value_layout == SUM_VALUE_FLAT && view.active_mask == nullptr) {
        if (view.null_mask == nullptr) {
            int32_t row = view.row_begin;
            for (; row + 3 < view.row_end; row += 4) {
                UpdatePackedRow(view, row, loadStateAt, storeStateAt, mode);
                UpdatePackedRow(view, row + 1, loadStateAt, storeStateAt, mode);
                UpdatePackedRow(view, row + 2, loadStateAt, storeStateAt, mode);
                UpdatePackedRow(view, row + 3, loadStateAt, storeStateAt, mode);
            }
            for (; row < view.row_end; ++row) {
                UpdatePackedRow(view, row, loadStateAt, storeStateAt, mode);
            }
            return;
        }
        for (int32_t row = view.row_begin; row < view.row_end; ++row) {
            if (!sum_detail::IsNullAt(view.null_mask, row)) {
                UpdatePackedRow(view, row, loadStateAt, storeStateAt, mode);
            }
        }
        return;
    }
    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (sum_detail::IsActiveAt(view.active_mask, row) &&
            !sum_detail::IsNullAt(view.null_mask, row)) {
            UpdatePackedRow(view, row, loadStateAt, storeStateAt, mode);
        }
    }
}

#if defined(__ARM_FEATURE_SVE)
template <typename LoadStateAt, typename StoreStateAt>
inline void WalkSve(
    const DecimalSumBatchView& view,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode mode) {
    if (view.active_mask == nullptr && view.null_mask == nullptr) {
        WalkScalar(view, loadStateAt, storeStateAt, mode);
        return;
    }
    const int32_t firstWord = view.row_begin / 32;
    const int32_t lastWord = (view.row_end + 31) / 32;
    for (int32_t word = firstWord; word < lastWord; ++word) {
        const int32_t wordBegin = word * 32;
        uint32_t validBits = 0xFFFFFFFFu;
        if (view.active_mask != nullptr) {
            validBits &= reinterpret_cast<const uint32_t*>(view.active_mask)[word];
        }
        if (view.null_mask != nullptr) {
            validBits &= ~reinterpret_cast<const uint32_t*>(view.null_mask)[word];
        }
        const int32_t begin = std::max(wordBegin, view.row_begin) - wordBegin;
        const int32_t end = std::min(wordBegin + 32, view.row_end) - wordBegin;
        const uint64_t range = ((1ULL << end) - 1ULL) & ~((1ULL << begin) - 1ULL);
        validBits &= static_cast<uint32_t>(range);
        if (validBits == 0) {
            continue;
        }
        const svbool_t predicate = sum_detail::LoadPredicate32(&validBits);
        uint32_t bits = sum_detail::StorePredicate32(predicate);
        while (bits != 0) {
            const int32_t lane = __builtin_ctz(bits);
            UpdatePackedRow(
                view,
                wordBegin + lane,
                loadStateAt,
                storeStateAt,
                mode);
            bits &= bits - 1;
        }
    }
}
#endif
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

/*
 * Grouped partial-result merge. Partial values are normalized by the engine
 * into DecimalSumPartial; NULL intermediate rows are ignored. All addresses
 * are validated before the first state store.
 */
template <typename PartialAt, typename NullAt, typename StateAddressAt, typename LoadStateAt, typename StoreStateAt>
int32_t DecimalSumMergeGrouped(
    int32_t numRows,
    PartialAt partialAt,
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
        decimal_sum_detail::ApplyPartial(state, partialAt(row), mode);
        storeStateAt(row, state);
    }
    if (stats.counted_rows != 0 && allSameState) {
        stats.groups_touched = 1;
    }
    sum_detail::StoreResult(stats, result);
    return SUM_OK;
}

/*
 * Packed decimal update. Untrusted views complete validation before the first
 * engine state store. The scalar walker and optional SVE bitmap walker both
 * preserve row order, so duplicate state addresses are safe.
 */
template <typename LoadStateAt, typename StoreStateAt>
int32_t DecimalSumRawPacked(
    const DecimalSumBatchView& view,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode overflowMode,
    SumExecutionMode executionMode,
    SumResult* result) {
    if (overflowMode != DECIMAL_SUM_WRAP_TRACK &&
        overflowMode != DECIMAL_SUM_STRICT_NULL) {
        return SUM_INVALID_ARGUMENT;
    }
    if (!decimal_sum_detail::IsInt128Available()) {
        return SUM_UNSUPPORTED;
    }

    decimal_sum_detail::DecimalBatchScan scan;
    if (!view.trusted || result != nullptr) {
        const auto status = decimal_sum_detail::ScanPacked(view, scan);
        if (status != SUM_OK) {
            return status;
        }
    }

    SumExecutionMode resolved = SUM_EXECUTION_SCALAR;
#if defined(__ARM_FEATURE_SVE)
    if (executionMode == SUM_EXECUTION_AUTO &&
        view.active_mask != nullptr && view.null_mask == nullptr &&
        view.row_end - view.row_begin >= 32 &&
        sum_detail::CanUsePackedSve()) {
        resolved = SUM_EXECUTION_SVE;
    }
#endif
    if (executionMode == SUM_EXECUTION_SCALAR) {
        resolved = SUM_EXECUTION_SCALAR;
    } else if (executionMode == SUM_EXECUTION_SVE) {
        resolved = SUM_EXECUTION_SVE;
    } else if (executionMode != SUM_EXECUTION_AUTO) {
        return SUM_INVALID_ARGUMENT;
    }

    if (resolved == SUM_EXECUTION_SVE) {
#if defined(__ARM_FEATURE_SVE)
        if (!sum_detail::CanUsePackedSve()) {
            return SUM_UNSUPPORTED;
        }
        decimal_sum_detail::WalkSve(
            view,
            loadStateAt,
            storeStateAt,
            overflowMode);
#else
        return SUM_UNSUPPORTED;
#endif
    } else {
        decimal_sum_detail::WalkScalar(
            view,
            loadStateAt,
            storeStateAt,
            overflowMode);
    }
    sum_detail::StoreResult(scan.stats, result);
    return SUM_OK;
}

template <typename LoadStateAt, typename StoreStateAt>
int32_t DecimalSumMergePacked(
    const DecimalSumBatchView& view,
    LoadStateAt loadStateAt,
    StoreStateAt storeStateAt,
    DecimalSumOverflowMode overflowMode,
    SumExecutionMode executionMode,
    SumResult* result) {
    return DecimalSumRawPacked(
        view,
        loadStateAt,
        storeStateAt,
        overflowMode,
        executionMode,
        result);
}

inline bool DecimalSumAdjustForOverflow(
    DecimalInt128 sum,
    int64_t overflow,
    DecimalInt128* result) {
    return decimal_sum_detail::AdjustForOverflow(sum, overflow, result);
}

} // namespace omni::subop

#endif /* OMNI_SUBOP_DECIMAL_SUM_BATCH_H */
