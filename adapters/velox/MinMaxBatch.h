#ifndef OMNI_SUBOP_ADAPTER_VELOX_MIN_MAX_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_MIN_MAX_BATCH_H

#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/min_max_batch.h"
#include "velox/vector/ConstantVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace min_max_detail {

template <typename IsInitializedAt, typename MarkInitializedAt>
int32_t TryStateBase(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int32_t stateOffset,
    omni::subop::MinMaxKind kind,
    IsInitializedAt isInitializedAt,
    MarkInitializedAt markInitializedAt,
    omni::subop::SumResult* result) {
    if (stateOffset < 0) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    if (args.size() != 1) {
        return omni::subop::SUM_UNSUPPORTED;
    }
    if (rows.size() > std::numeric_limits<int32_t>::max()) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    if (rows.size() > 0 && groups == nullptr) {
        return omni::subop::SUM_INVALID_STATE;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    const auto* base = args[0]->loadedVector();
    omni::subop::SumInt64StateBaseView view;
    view.num_rows = static_cast<int32_t>(rows.size());
    view.row_begin = static_cast<int32_t>(rows.begin());
    view.row_end = static_cast<int32_t>(rows.end());
    view.state_offset = stateOffset;
    view.row_bases = groups;
    view.active_mask = rows.isAllSelected() ? nullptr : rows.allBits();
    view.trusted = true;

    if (base->isFlatEncoding()) {
        const auto* flat =
            base->as<facebook::velox::FlatVector<int64_t>>();
        if (flat == nullptr) {
            return omni::subop::SUM_UNSUPPORTED;
        }
        if (flat->size() > std::numeric_limits<int32_t>::max() ||
            flat->size() < rows.end()) {
            return omni::subop::SUM_INVALID_ARGUMENT;
        }
        view.value_layout = omni::subop::SUM_VALUE_FLAT;
        view.value_count = static_cast<int32_t>(flat->size());
        view.values = flat->rawValues();
        view.valid_mask = flat->rawNulls();
        return omni::subop::MinMaxStateBasePacked(
            view,
            kind,
            isInitializedAt,
            markInitializedAt,
            result);
    }

    if (base->encoding() ==
        facebook::velox::VectorEncoding::Simple::CONSTANT) {
        const auto* constant =
            base->as<facebook::velox::ConstantVector<int64_t>>();
        if (constant == nullptr) {
            return omni::subop::SUM_UNSUPPORTED;
        }
        if (base->isNullAt(0)) {
            if (result != nullptr) {
                omni::subop::SumResult stats{};
                stats.selected_rows = rows.countSelected();
                stats.null_skipped_rows = stats.selected_rows;
                *result = stats;
            }
            return omni::subop::SUM_OK;
        }
        const int64_t value = constant->value();
        view.value_layout = omni::subop::SUM_VALUE_CONSTANT;
        view.value_count = 1;
        view.values = &value;
        view.valid_mask = nullptr;
        return omni::subop::MinMaxStateBasePacked(
            view,
            kind,
            isInitializedAt,
            markInitializedAt,
            result);
    }

    return omni::subop::SUM_UNSUPPORTED;
}

inline int32_t BuildPackedView(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    facebook::velox::DecodedVector& decoded,
    int32_t stateOffset,
    std::vector<int64_t*>& stateValues,
    std::vector<uint64_t>& nullWords,
    omni::subop::Int64BatchView& view) {
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
    if (args.size() != 1) {
        return omni::subop::SUM_INVALID_ARGUMENT;
    }
    auto initializedAtRow = [&](int32_t row) {
        return isInitializedAt(groups[row]);
    };
    auto markAtRow = [&](int32_t row) {
        markInitializedAt(groups[row]);
    };
    const auto stateBaseStatus = min_max_detail::TryStateBase(
        groups,
        rows,
        args,
        stateOffset,
        kind,
        initializedAtRow,
        markAtRow,
        result);
    if (stateBaseStatus != omni::subop::SUM_UNSUPPORTED) {
        return stateBaseStatus;
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    facebook::velox::DecodedVector decoded(*args[0], rows);
    std::vector<int64_t*> stateValues;
    std::vector<uint64_t> nullWords;
    omni::subop::Int64BatchView view;
    const auto status = min_max_detail::BuildPackedView(
        groups, rows, decoded, stateOffset, stateValues, nullWords, view);
    if (status != omni::subop::SUM_OK) {
        return status;
    }
    return omni::subop::MinMaxRawPacked(
        view,
        initializedAtRow,
        markAtRow,
        kind,
        omni::subop::MIN_MAX_AUTO,
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
