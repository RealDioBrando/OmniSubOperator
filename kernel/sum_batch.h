#ifndef OMNI_SUBOP_SUM_BATCH_H
#define OMNI_SUBOP_SUM_BATCH_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#if defined(__ARM_FEATURE_SVE)
#include <arm_sve.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif
#endif

namespace omni::subop {

enum SumStatus {
    SUM_OK = 0,
    SUM_INVALID_ARGUMENT = -1,
    SUM_UNSUPPORTED = -2,
    SUM_INVALID_STATE = -3,
    SUM_OVERFLOW = -4,
    SUM_INTERNAL_ERROR = -5,
};

enum SumValueLayout {
    SUM_VALUE_FLAT = 0,
    SUM_VALUE_CONSTANT = 1,
    SUM_VALUE_DICTIONARY = 2,
};

enum SumExecutionMode {
    SUM_EXECUTION_AUTO = 0,
    SUM_EXECUTION_SCALAR = 1,
    SUM_EXECUTION_SVE = 2,
};

struct SumResult {
    uint64_t selected_rows = 0;
    uint64_t counted_rows = 0;
    uint64_t null_skipped_rows = 0;
    uint32_t groups_touched = 0;
};

/*
 * Engine-independent packed view for one int64 SUM batch.
 *
 * active_mask and null_mask use 64-bit words. Bit 0 is row 0. A set active
 * bit means the row is selected. A set null bit means the row contributes no
 * value. A NULL active_mask means every row is active. A NULL null_mask means
 * there are no NULL rows.
 *
 * state_values is indexed by engine row and contains the direct address of the
 * engine-owned int64 accumulator. Entries outside active_mask/non-null rows
 * may be NULL. The kernel validates every row it will update before it writes
 * any state.
 *
 * value_indices is used only for SUM_VALUE_DICTIONARY. value_count and
 * index_count are explicit so the kernel can reject malformed views before
 * touching engine state.
 *
 * trusted means the adapter has already validated every selected state pointer,
 * dictionary index, and value range. A trusted view may skip the duplicate
 * validation/statistics scan when the caller does not request SumResult.
 * Untrusted views always take the no-write validation path. track_groups asks
 * for an exact groups_touched count; otherwise groups_touched is either 1 for
 * a single shared state or 0 when the count was not tracked.
 */
struct SumInt64BatchView {
    int32_t num_rows = 0;
    int32_t row_begin = 0;
    int32_t row_end = 0;
    int32_t value_count = 0;
    int32_t index_count = 0;
    SumValueLayout value_layout = SUM_VALUE_FLAT;
    const int64_t* values = nullptr;
    const int32_t* value_indices = nullptr;
    int64_t* const* state_values = nullptr;
    const uint64_t* active_mask = nullptr;
    const uint64_t* null_mask = nullptr;
    bool track_groups = false;
    bool trusted = false;
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

inline bool IsActiveAt(const uint64_t* mask, int32_t row) {
    return mask == nullptr ||
        ((mask[row / 64] >> (row % 64)) & 1ULL) != 0;
}

inline bool IsNullAt(const uint64_t* mask, int32_t row) {
    return mask != nullptr &&
        ((mask[row / 64] >> (row % 64)) & 1ULL) != 0;
}

inline int64_t LoadValue(const SumInt64BatchView& view, int32_t row) {
    if (view.value_layout == SUM_VALUE_CONSTANT) {
        return view.values[0];
    }
    if (view.value_layout == SUM_VALUE_DICTIONARY) {
        return view.values[view.value_indices[row]];
    }
    return view.values[row];
}

inline int32_t ValidateBatchView(const SumInt64BatchView& view) {
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
        if (!IsActiveAt(view.active_mask, row) || IsNullAt(view.null_mask, row)) {
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

struct BatchScan {
    SumResult stats{};
    bool firstState = true;
    bool allSameState = true;
    int64_t* firstStateValue = nullptr;
};

inline int32_t ScanBatch(
    const SumInt64BatchView& view,
    BatchScan& scan) {
    const auto status = ValidateBatchView(view);
    if (status != SUM_OK) {
        return status;
    }

    std::unordered_set<int64_t*> groups;
    if (view.track_groups) {
        groups.reserve(static_cast<size_t>(view.row_end - view.row_begin));
    }

    for (int32_t row = view.row_begin; row < view.row_end; ++row) {
        if (!IsActiveAt(view.active_mask, row)) {
            continue;
        }
        ++scan.stats.selected_rows;
        if (IsNullAt(view.null_mask, row)) {
            ++scan.stats.null_skipped_rows;
            continue;
        }

        ++scan.stats.counted_rows;
        int64_t* state = view.state_values[row];
        if (scan.firstState) {
            scan.firstState = false;
            scan.firstStateValue = state;
        } else if (state != scan.firstStateValue) {
            scan.allSameState = false;
        }
        if (view.track_groups) {
            groups.insert(state);
        }
    }

    if (view.track_groups) {
        scan.stats.groups_touched = static_cast<uint32_t>(groups.size());
    } else if (scan.stats.counted_rows != 0 && scan.allSameState) {
        scan.stats.groups_touched = 1;
    }
    return SUM_OK;
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void UpdatePackedRow(
    const SumInt64BatchView& view,
    int32_t row,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    int64_t* state = view.state_values[row];
    const int64_t value = LoadValue(view, row);
    if (stateInitializedAt(row)) {
        *state = WrapAdd(*state, value);
    } else {
        *state = value;
    }
    markInitializedAt(row);
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void UpdatePackedFlatRow(
    const SumInt64BatchView& view,
    int32_t row,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    int64_t* state = view.state_values[row];
    const int64_t value = view.values[row];
    if (stateInitializedAt(row)) {
        *state = WrapAdd(*state, value);
    } else {
        *state = value;
    }
    markInitializedAt(row);
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void SumPackedScalarRows(
    const SumInt64BatchView& view,
    int32_t rowBegin,
    int32_t rowEnd,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    if (view.value_layout == SUM_VALUE_FLAT && view.active_mask == nullptr) {
        if (view.null_mask == nullptr) {
            for (int32_t row = rowBegin; row < rowEnd; ++row) {
                UpdatePackedFlatRow(view, row, stateInitializedAt, markInitializedAt);
            }
            return;
        }
        for (int32_t row = rowBegin; row < rowEnd; ++row) {
            if (!IsNullAt(view.null_mask, row)) {
                UpdatePackedFlatRow(view, row, stateInitializedAt, markInitializedAt);
            }
        }
        return;
    }

    for (int32_t row = rowBegin; row < rowEnd; ++row) {
        if (IsActiveAt(view.active_mask, row) && !IsNullAt(view.null_mask, row)) {
            UpdatePackedRow(view, row, stateInitializedAt, markInitializedAt);
        }
    }
}

#if defined(__ARM_FEATURE_SVE)
inline bool CanUsePackedSve() {
    static const bool canUse = []() {
#if defined(__linux__)
#ifndef HWCAP_SVE
        constexpr unsigned long kHwcapSve = 1UL << 22;
#else
        constexpr unsigned long kHwcapSve = HWCAP_SVE;
#endif
        if ((getauxval(AT_HWCAP) & kHwcapSve) == 0) {
            return false;
        }
#endif
        return svcntb() == 32;
    }();
    return canUse;
}

inline svbool_t LoadPredicate32(const uint32_t* bits) {
    svbool_t predicate;
    __asm__ __volatile__("ldr %0, [%1]"
                         : "=Upl"(predicate)
                         : "r"(bits)
                         : "memory");
    return predicate;
}

inline uint32_t StorePredicate32(svbool_t predicate) {
    uint32_t bits = 0;
    __asm__ __volatile__("str %1, [%0]"
                         : : "r"(&bits), "Upl"(predicate)
                         : "memory");
    return bits;
}

template <typename StateInitializedAt, typename MarkInitializedAt>
inline void SumPackedSveRows(
    const SumInt64BatchView& view,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt) {
    if (view.active_mask == nullptr && view.null_mask == nullptr) {
        SumPackedScalarRows(
            view,
            view.row_begin,
            view.row_end,
            stateInitializedAt,
            markInitializedAt);
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

        const int32_t rangeBegin = std::max(wordBegin, view.row_begin) - wordBegin;
        const int32_t rangeEnd = std::min(wordBegin + 32, view.row_end) - wordBegin;
        const uint64_t rangeMask =
            ((1ULL << rangeEnd) - 1ULL) &
            ~((1ULL << rangeBegin) - 1ULL);
        validBits &= static_cast<uint32_t>(rangeMask);
        if (validBits == 0) {
            continue;
        }

        const svbool_t predicate = LoadPredicate32(&validBits);
        const uint32_t activeBits = StorePredicate32(predicate);
        if (activeBits == 0) {
            continue;
        }
        if (activeBits == 0xFFFFFFFFu) {
            SumPackedScalarRows(
                view,
                wordBegin,
                wordBegin + 32,
                stateInitializedAt,
                markInitializedAt);
            continue;
        }

        uint32_t bits = activeBits;
        while (bits != 0) {
            const int32_t lane = __builtin_ctz(bits);
            UpdatePackedRow(
                view,
                wordBegin + lane,
                stateInitializedAt,
                markInitializedAt);
            bits &= bits - 1;
        }
    }
}
#endif

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
 * Grouped raw update using the conservative pointer-to-delta map. This entry
 * point remains the reference implementation used by tests and by callers
 * that need an exact groups_touched count.
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
 * Direct grouped raw update for engine adapters that already expose state
 * initialization. Validation is separated from mutation, so an invalid state
 * pointer discovered anywhere in the batch still produces a no-write failure.
 *
 * Unlike SumRawGrouped(), groups_touched is exact only when the batch contains
 * one state. A return value of zero means "not tracked", not "no groups".
 */
template <typename ValueAt, typename NullAt, typename StateValueAt, typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumRawGroupedDirect(
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

    bool firstState = true;
    bool allSameState = true;
    int64_t* firstStateValue = nullptr;
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
        if (firstState) {
            firstState = false;
            firstStateValue = state;
        } else if (state != firstStateValue) {
            allSameState = false;
        }
        ++stats.counted_rows;
    }

    for (int32_t row = 0; row < numRows; ++row) {
        if (nullAt(row)) {
            continue;
        }
        int64_t* state = stateValueAt(row);
        const int64_t value = valueAt(row);
        if (stateInitializedAt(row)) {
            *state = sum_detail::WrapAdd(*state, value);
        } else {
            *state = value;
        }
        markInitializedAt(row);
    }

    if (stats.counted_rows != 0 && allSameState) {
        stats.groups_touched = 1;
    }
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

/*
 * Packed direct update. This is the performance-oriented adapter boundary.
 * Untrusted views validate all selected state pointers and dictionary indices
 * before the first write; trusted views rely on the adapter's equivalent
 * validation. The selected update is then handled by the scalar walker or the
 * SVE bitmap walker.
 *
 * Duplicate state pointers are intentionally handled row-by-row in order. The
 * same state can therefore receive multiple contributions from one mask word
 * without a pointer-keyed delta map.
 */
template <typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumRawPacked(
    const SumInt64BatchView& view,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumExecutionMode mode,
    SumResult* result) {
    if (mode != SUM_EXECUTION_AUTO && mode != SUM_EXECUTION_SCALAR &&
        mode != SUM_EXECUTION_SVE) {
        return SUM_INVALID_ARGUMENT;
    }

    sum_detail::BatchScan scan;
    if (!view.trusted || result != nullptr) {
        const int32_t scanStatus = sum_detail::ScanBatch(view, scan);
        if (scanStatus != SUM_OK) {
            return scanStatus;
        }
    }

    SumExecutionMode resolvedMode = mode;
    if (mode == SUM_EXECUTION_AUTO) {
        resolvedMode = SUM_EXECUTION_SCALAR;
#if defined(__ARM_FEATURE_SVE)
        if (view.active_mask != nullptr && view.null_mask == nullptr &&
            view.row_end - view.row_begin >= 32 &&
            sum_detail::CanUsePackedSve()) {
            resolvedMode = SUM_EXECUTION_SVE;
        }
#endif
    }

    if (resolvedMode == SUM_EXECUTION_SVE) {
#if defined(__ARM_FEATURE_SVE)
        if (!sum_detail::CanUsePackedSve()) {
            return SUM_UNSUPPORTED;
        }
        sum_detail::SumPackedSveRows(
            view,
            stateInitializedAt,
            markInitializedAt);
#else
        return SUM_UNSUPPORTED;
#endif
    } else {
        sum_detail::SumPackedScalarRows(
            view,
            view.row_begin,
            view.row_end,
            stateInitializedAt,
            markInitializedAt);
    }

    sum_detail::StoreResult(scan.stats, result);
    return SUM_OK;
}

template <typename StateInitializedAt, typename MarkInitializedAt>
int32_t SumMergePacked(
    const SumInt64BatchView& view,
    StateInitializedAt stateInitializedAt,
    MarkInitializedAt markInitializedAt,
    SumExecutionMode mode,
    SumResult* result) {
    return SumRawPacked(
        view,
        stateInitializedAt,
        markInitializedAt,
        mode,
        result);
}

} // namespace omni::subop

#endif /* OMNI_SUBOP_SUM_BATCH_H */