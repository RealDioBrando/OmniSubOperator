#ifndef OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/sum_batch.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"
#include "velox/vector/BaseVector.h"

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
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() != 1 ||
        (rows.size() > 0 && groups == nullptr)) {
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
    return omni::subop::SumRawGrouped(
        numRows,
        [&](int32_t row) {
            return decoded.valueAt<int64_t>(engineRow(row));
        },
        [&](int32_t row) {
            return decoded.isNullAt(engineRow(row));
        },
        [&](int32_t row) {
            return reinterpret_cast<int64_t*>(groups[engineRow(row)] + stateOffset);
        },
        [&](int32_t row) {
            return isInitializedAt(groups[engineRow(row)]);
        },
        [&](int32_t row) {
            markInitializedAt(groups[engineRow(row)]);
        },
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
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() != 1 ||
        (rows.size() > 0 && groups == nullptr)) {
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
    return omni::subop::SumMergeGrouped(
        numRows,
        [&](int32_t row) {
            return decoded.valueAt<int64_t>(engineRow(row));
        },
        [&](int32_t row) {
            return decoded.isNullAt(engineRow(row));
        },
        [&](int32_t row) {
            return reinterpret_cast<int64_t*>(groups[engineRow(row)] + stateOffset);
        },
        [&](int32_t row) {
            return isInitializedAt(groups[engineRow(row)]);
        },
        [&](int32_t row) {
            markInitializedAt(groups[engineRow(row)]);
        },
        result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_SUM_BATCH_H */
