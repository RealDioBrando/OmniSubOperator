#ifndef OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/sum_batch.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace sum_detail {

inline void FillSelected(
    const facebook::velox::SelectivityVector& rows,
    std::vector<int32_t>& selected) {
    selected.clear();
    if (rows.isAllSelected()) {
        return;
    }
    selected.reserve(static_cast<size_t>(rows.countSelected()));
    rows.applyToSelected([&](auto row) {
        selected.push_back(static_cast<int32_t>(row));
    });
}

inline int32_t EngineRow(
    const std::vector<int32_t>& selected,
    bool allSelected,
    int32_t row) {
    return allSelected ? row : selected[static_cast<size_t>(row)];
}

/*
 * Convert a Velox decoded vector into the engine-independent packed view.
 * The decoded vector owns the value/indices lifetime; stateValues and
 * nullWords are backed by the caller's vectors for the duration of the call.
 */
inline int32_t BuildPackedView(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    facebook::velox::DecodedVector& decoded,
    int32_t stateOffset,
    std::vector<int64_t*>& stateValues,
    std::vector<uint64_t>& nullWords,
    omni::subop::SumInt64BatchView& view) {
    if (rows.size() > std::numeric_limits<int32_t>::max() ||
        (rows.size() > 0 && groups == nullptr)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    const auto rowCount = static_cast<int32_t>(rows.size());
    const auto wordCount = (static_cast<size_t>(rowCount) + 63) / 64;
    stateValues.assign(static_cast<size_t>(rowCount), nullptr);
    nullWords.clear();

    const bool hasNulls = decoded.mayHaveNulls();
    if (hasNulls) {
        nullWords.assign(wordCount, 0);
    }

    for (int32_t row = rows.begin(); row < rows.end(); ++row) {
        if (!rows.isValid(row)) {
            continue;
        }
        if (hasNulls && decoded.isNullAt(row)) {
            nullWords[static_cast<size_t>(row) / 64] |=
                1ULL << (static_cast<size_t>(row) % 64);
            continue;
        }
        char* group = groups[row];
        if (group == nullptr) {
            return omni::subop::SUM_INVALID_STATE;
        }
        stateValues[static_cast<size_t>(row)] =
            reinterpret_cast<int64_t*>(group + stateOffset);
    }

    const auto* base = decoded.base();
    view = {};
    view.num_rows = rowCount;
    view.row_begin = static_cast<int32_t>(rows.begin());
    view.row_end = static_cast<int32_t>(rows.end());
    view.value_count = base == nullptr ? rowCount : static_cast<int32_t>(base->size());
    view.index_count = rowCount;
    view.value_layout = decoded.isConstantMapping()
        ? omni::subop::SUM_VALUE_CONSTANT
        : (decoded.isIdentityMapping()
               ? omni::subop::SUM_VALUE_FLAT
               : omni::subop::SUM_VALUE_DICTIONARY);
    view.values = decoded.data<int64_t>();
    view.value_indices = view.value_layout == omni::subop::SUM_VALUE_DICTIONARY
        ? decoded.indices()
        : nullptr;
    view.state_values = stateValues.empty() ? nullptr : stateValues.data();
    view.active_mask = rowCount == 0 || rows.isAllSelected() ? nullptr : rows.allBits();
    view.null_mask = nullWords.empty() ? nullptr : nullWords.data();
    view.track_groups = false;
    view.trusted = true;

    if (view.values == nullptr || view.value_count < 1) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    if (view.value_layout == omni::subop::SUM_VALUE_FLAT &&
        view.value_count < view.row_end) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    if (view.value_layout == omni::subop::SUM_VALUE_DICTIONARY &&
        (view.value_indices == nullptr || view.index_count < view.row_end)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    return omni::subop::SUM_OK;
}

} // namespace sum_detail

/*
 * Sum int64 raw values for grouped aggregation. The engine owns group rows,
 * accumulator initialization, and NULL semantics; this adapter only decodes
 * values and maps selected rows onto those engine-owned states.
 */
template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t SumRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (args.size() != 1) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int64_t*> stateValues;
    std::vector<uint64_t> nullWords;
    omni::subop::SumInt64BatchView view;
    const auto status = sum_detail::BuildPackedView(
        groups,
        rows,
        decoded,
        stateOffset,
        stateValues,
        nullWords,
        view);
    if (status != omni::subop::SUM_OK) {
        return status;
    }
    auto initializedAtRow = [&](int32_t row) {
        return isInitializedAt(groups[row]);
    };
    auto markInitializedAtRow = [&](int32_t row) {
        markInitializedAt(groups[row]);
    };
    return omni::subop::SumRawPacked(
        view,
        initializedAtRow,
        markInitializedAtRow,
        omni::subop::SUM_EXECUTION_AUTO,
        result);
}

template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t SumRawSingleGroup(
    char* group,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() != 1 ||
        (rows.size() > 0 && group == nullptr)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int32_t> selected;
    sum_detail::FillSelected(rows, selected);
    const bool allSelected = selected.empty() && rows.isAllSelected();
    const auto numRows = allSelected
        ? static_cast<int32_t>(rows.size())
        : static_cast<int32_t>(selected.size());

    auto engineRow = [&](int32_t row) {
        return sum_detail::EngineRow(selected, allSelected, row);
    };
    bool initialized = isInitializedAt(group);
    const auto status = omni::subop::SumRawSingleGroup(
        numRows,
        [&](int32_t row) {
            return decoded.valueAt<int64_t>(engineRow(row));
        },
        [&](int32_t row) {
            return decoded.isNullAt(engineRow(row));
        },
        reinterpret_cast<int64_t*>(group + stateOffset),
        initialized,
        &initialized,
        result);
    if (status == omni::subop::SUM_OK && initialized && !isInitializedAt(group)) {
        markInitializedAt(group);
    }
    return status;
}

template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t SumMergeBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (args.size() != 1) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int64_t*> stateValues;
    std::vector<uint64_t> nullWords;
    omni::subop::SumInt64BatchView view;
    const auto status = sum_detail::BuildPackedView(
        groups,
        rows,
        decoded,
        stateOffset,
        stateValues,
        nullWords,
        view);
    if (status != omni::subop::SUM_OK) {
        return status;
    }
    auto initializedAtRow = [&](int32_t row) {
        return isInitializedAt(groups[row]);
    };
    auto markInitializedAtRow = [&](int32_t row) {
        markInitializedAt(groups[row]);
    };
    return omni::subop::SumMergePacked(
        view,
        initializedAtRow,
        markInitializedAtRow,
        omni::subop::SUM_EXECUTION_AUTO,
        result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H */