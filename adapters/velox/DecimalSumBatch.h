#ifndef OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H

#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "kernel/decimal_sum_batch.h"
#include "velox/vector/ComplexVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace decimal_sum_detail {

template <typename TInputType>
inline int32_t BuildPackedView(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    facebook::velox::DecodedVector& decoded,
    int32_t stateOffset,
    std::vector<omni::subop::DecimalInt128>& values,
    std::vector<const void*>& stateValues,
    std::vector<uint64_t>& nullWords,
    omni::subop::DecimalSumBatchView& view) {
    if (stateOffset < 0 ||
        rows.size() > std::numeric_limits<int32_t>::max() ||
        (rows.size() > 0 && groups == nullptr)) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    const auto rowCount = static_cast<int32_t>(rows.size());
    const auto wordCount = (static_cast<size_t>(rowCount) + 63) / 64;
    values.assign(static_cast<size_t>(rowCount), 0);
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
            static_cast<const void*>(group + stateOffset);
        values[static_cast<size_t>(row)] = static_cast<omni::subop::DecimalInt128>(
            decoded.valueAt<TInputType>(row));
    }

    view = {};
    view.num_rows = rowCount;
    view.row_begin = static_cast<int32_t>(rows.begin());
    view.row_end = static_cast<int32_t>(rows.end());
    view.value_count = rowCount;
    view.value_layout = omni::subop::SUM_VALUE_FLAT;
    view.values = values.empty() ? nullptr : values.data();
    view.state_values = stateValues.empty() ? nullptr : stateValues.data();
    view.active_mask = rowCount == 0 || rows.isAllSelected() ? nullptr : rows.allBits();
    view.null_mask = nullWords.empty() ? nullptr : nullWords.data();
    view.trusted = true;
    return omni::subop::SUM_OK;
}

} // namespace decimal_sum_detail

/*
 * Decimal raw-input adapter. The engine remains the owner of group rows,
 * accumulator layout, scaling, final overflow adjustment, and partial merge.
 */
template <
    typename TInputType,
    typename AccumulatorType,
    typename MarkInitializedAt>
int32_t DecimalSumRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (args.size() != 1) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<omni::subop::DecimalInt128> values;
    std::vector<const void*> stateValues;
    std::vector<uint64_t> nullWords;
    omni::subop::DecimalSumBatchView view;
    const auto status = decimal_sum_detail::BuildPackedView<TInputType>(
        groups,
        rows,
        decoded,
        stateOffset,
        values,
        stateValues,
        nullWords,
        view);
    if (status != omni::subop::SUM_OK) {
        return status;
    }

    auto stateAt = [&](int32_t row) -> AccumulatorType* {
        return reinterpret_cast<AccumulatorType*>(
            const_cast<void*>(stateValues[static_cast<size_t>(row)]));
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
            using VeloxInt128 =
                std::remove_reference_t<decltype(*accumulator->sum)>;
            accumulator->sum = static_cast<VeloxInt128>(state.sum);
        }
        if (state.count != 0 || state.invalid) {
            markInitializedAt(groups[row]);
        }
    };

    return omni::subop::DecimalSumRawPacked(
        view,
        loadStateAt,
        storeStateAt,
        omni::subop::DECIMAL_SUM_WRAP_TRACK,
        omni::subop::SUM_EXECUTION_AUTO,
        result);
}

template <typename TInputType, typename AccumulatorType>
int32_t DecimalSumRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::SumResult* result) {
    return DecimalSumRawBatch<TInputType, AccumulatorType>(
        groups,
        rows,
        args,
        stateOffset,
        [](char*) {},
        result);
}

/*
 * Decimal partial-result merge adapter. Velox represents the intermediate as
 * Row<TSumType, bool>: child 0 is the partial sum (NULL means overflow) and
 * child 1 marks an empty producer. Only flat row vectors are accepted; other
 * encodings fall back to the engine's native merge.
 */
template <
    typename TSumType,
    typename AccumulatorType,
    typename MarkInitializedAt>
int32_t DecimalSumMergeBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (stateOffset < 0 || args.size() != 1 || args[0] == nullptr ||
        args[0]->encoding() != facebook::velox::VectorEncoding::Simple::ROW ||
        (rows.size() > 0 && groups == nullptr)) {
        return omni::subop::SUM_UNSUPPORTED;
    }
    const auto* rowVector =
        args[0]->as<facebook::velox::RowVector>();
    if (rowVector == nullptr || rowVector->childrenSize() < 2) {
        return omni::subop::SUM_UNSUPPORTED;
    }

    facebook::velox::DecodedVector sum(*rowVector->childAt(0), rows);
    facebook::velox::DecodedVector empty(*rowVector->childAt(1), rows);
    const auto rowCount = static_cast<int32_t>(rows.size());
    const auto wordCount = (static_cast<size_t>(rowCount) + 63) / 64;
    std::vector<omni::subop::DecimalSumPartial> partials(
        static_cast<size_t>(rowCount));
    std::vector<const void*> stateValues(static_cast<size_t>(rowCount), nullptr);
    std::vector<uint64_t> nullWords;
    const bool hasNulls = args[0]->mayHaveNulls();
    const bool hasInactiveRows = !rows.isAllSelected();
    if (hasNulls || hasInactiveRows) {
        nullWords.assign(wordCount, 0);
    }
    for (int32_t row = rows.begin(); row < rows.end(); ++row) {
        if (!rows.isValid(row) ||
            (hasNulls && args[0]->isNullAt(row))) {
            nullWords[static_cast<size_t>(row) / 64] |=
                1ULL << (static_cast<size_t>(row) % 64);
            continue;
        }
        char* group = groups[row];
        if (group == nullptr) {
            return omni::subop::SUM_INVALID_STATE;
        }
        stateValues[static_cast<size_t>(row)] =
            static_cast<const void*>(group + stateOffset);
        const bool isEmpty = empty.isNullAt(row) || empty.valueAt<bool>(row);
        if (isEmpty) {
            partials[static_cast<size_t>(row)].count = 0;
        } else if (sum.isNullAt(row)) {
            partials[static_cast<size_t>(row)].count = -1;
        } else {
            partials[static_cast<size_t>(row)].sum =
                static_cast<omni::subop::DecimalInt128>(
                    sum.valueAt<TSumType>(row));
            partials[static_cast<size_t>(row)].count = 1;
        }
    }

    auto stateAt = [&](int32_t row) -> AccumulatorType* {
        return reinterpret_cast<AccumulatorType*>(
            const_cast<void*>(stateValues[static_cast<size_t>(row)]));
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
            using VeloxInt128 =
                std::remove_reference_t<decltype(*accumulator->sum)>;
            accumulator->sum = static_cast<VeloxInt128>(state.sum);
        }
        if (state.count != 0 || state.invalid) {
            markInitializedAt(groups[row]);
        }
    };
    auto addressAt = [&](int32_t row) -> const void* {
        return stateValues[static_cast<size_t>(row)];
    };
    return omni::subop::DecimalSumMergeGrouped(
        rowCount,
        [&](int32_t row) { return partials[static_cast<size_t>(row)]; },
        [&](int32_t row) {
            return !nullWords.empty() &&
                ((nullWords[static_cast<size_t>(row) / 64] >>
                  (static_cast<size_t>(row) % 64)) & 1ULL) != 0;
        },
        addressAt,
        loadStateAt,
        storeStateAt,
        omni::subop::DECIMAL_SUM_WRAP_TRACK,
        result);
}

template <typename TSumType, typename AccumulatorType>
int32_t DecimalSumMergeBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::SumResult* result) {
    return DecimalSumMergeBatch<TSumType, AccumulatorType>(
        groups,
        rows,
        args,
        stateOffset,
        [](char*) {},
        result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_DECIMAL_SUM_BATCH_H */
