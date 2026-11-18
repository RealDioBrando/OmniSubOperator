#ifndef OMNI_SUBOP_ADAPTER_VELOX_MIN_MAX_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_MIN_MAX_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/min_max_batch.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace min_max_detail {

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

} // namespace min_max_detail

template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t MinMaxRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::MinMaxKind kind,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() ||
        args.size() != 1 || (rows.size() > 0 && groups == nullptr)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int32_t> selected;
    min_max_detail::FillSelected(rows, selected);
    const bool allSelected = selected.empty() && rows.isAllSelected();
    const auto numRows = allSelected
        ? static_cast<int32_t>(rows.size())
        : static_cast<int32_t>(selected.size());

    auto engineRow = [&](int32_t row) {
        return min_max_detail::EngineRow(selected, allSelected, row);
    };
    return omni::subop::MinMaxRawGroupedDirect(
        numRows,
        [&](int32_t row) { return decoded.valueAt<int64_t>(engineRow(row)); },
        [&](int32_t row) { return decoded.isNullAt(engineRow(row)); },
        [&](int32_t row) {
            char* group = groups[engineRow(row)];
            return group == nullptr
                ? nullptr
                : reinterpret_cast<int64_t*>(group + stateOffset);
        },
        [&](int32_t row) { return isInitializedAt(groups[engineRow(row)]); },
        [&](int32_t row) { markInitializedAt(groups[engineRow(row)]); },
        kind,
        result);
}

template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t MinMaxMergeBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::MinMaxKind kind,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    return MinMaxRawBatch(
        groups,
        rows,
        args,
        stateOffset,
        kind,
        isInitializedAt,
        markInitializedAt,
        result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_MIN_MAX_BATCH_H */
