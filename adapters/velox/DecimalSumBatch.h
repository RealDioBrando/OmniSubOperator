#ifndef OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "kernel/decimal_sum_batch.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace decimal_sum_detail {

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


} // namespace decimal_sum_detail

/*
 * Decimal raw-input adapter. The engine remains the owner of group rows,
 * accumulator layout, scaling, final overflow adjustment, and partial merge.
 * Merge continues through the native path.
 */
template <typename TInputType, typename AccumulatorType>
int32_t DecimalSumRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::SumResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() ||
        args.size() != 1 || (rows.size() > 0 && groups == nullptr)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int32_t> selected;
    decimal_sum_detail::FillSelected(rows, selected);
    const bool allSelected = selected.empty() && rows.isAllSelected();
    const auto numRows = allSelected
        ? static_cast<int32_t>(rows.size())
        : static_cast<int32_t>(selected.size());

    auto engineRow = [&](int32_t row) {
        return decimal_sum_detail::EngineRow(selected, allSelected, row);
    };

    auto stateAt = [&](int32_t row) -> AccumulatorType* {
        char* group = groups[engineRow(row)];
        if (group == nullptr) {
            return nullptr;
        }
        return reinterpret_cast<AccumulatorType*>(group + stateOffset);
    };

    auto valueAt = [&](int32_t row) -> omni::subop::DecimalInt128 {
        return static_cast<omni::subop::DecimalInt128>(
            decoded.valueAt<TInputType>(engineRow(row)));
    };
    auto nullAt = [&](int32_t row) {
        return decoded.isNullAt(engineRow(row));
    };
    auto addressAt = [&](int32_t row) -> const void* {
        return stateAt(row);
    };
    auto loadStateAt = [&](int32_t row) {
        const auto* accumulator = stateAt(row);
        omni::subop::DecimalSumState state;
        state.sum = accumulator->sum.has_value()
            ? static_cast<omni::subop::DecimalInt128>(*accumulator->sum)
            : 0;
        state.overflow = accumulator->overflow;
        state.count = accumulator->isEmpty ? 0 : 1;
        state.invalid = !accumulator->sum.has_value();
        return state;
    };
    auto storeStateAt = [&](int32_t row, omni::subop::DecimalSumState state) {
        auto* accumulator = stateAt(row);
        accumulator->overflow = state.overflow;
        accumulator->isEmpty = state.count == 0;
        if (state.invalid) {
            accumulator->sum.reset();
        } else {
            using VeloxInt128 = std::remove_reference_t<decltype(*accumulator->sum)>;
            accumulator->sum = static_cast<VeloxInt128>(state.sum);
        }
    };

    return omni::subop::DecimalSumRawGrouped(
        numRows,
        valueAt,
        nullAt,
        addressAt,
        loadStateAt,
        storeStateAt,
        omni::subop::DECIMAL_SUM_WRAP_TRACK,
        result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H */