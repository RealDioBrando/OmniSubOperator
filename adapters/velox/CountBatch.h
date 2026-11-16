/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Phase 0 Velox adapter for the count micro-operator.
 *
 * This header is included by the Velox aggregate implementation. The caller
 * supplies a StateAt(row) callable that resolves value<int64_t>(groups[row]).
 * Keeping that callable in the engine class avoids duplicating Velox's
 * accumulator offset logic in OmniSubOperator.
 */

#ifndef OMNI_SUBOP_ADAPTER_VELOX_COUNT_BATCH_H
#define OMNI_SUBOP_ADAPTER_VELOX_COUNT_BATCH_H

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#include "kernel/count_batch.h"
#include "velox/vector/BaseVector.h"
#include "velox/vector/ConstantVector.h"
#include "velox/vector/DecodedVector.h"
#include "velox/vector/FlatVector.h"
#include "velox/vector/LazyVector.h"
#include "velox/vector/SelectivityVector.h"

namespace omni::subop::velox {

namespace count_detail {

inline void FillActive(
    const facebook::velox::SelectivityVector& rows,
    std::vector<uint8_t>& active) {
    active.assign(static_cast<size_t>(rows.size()), 0);
    if (rows.isAllSelected()) {
        std::fill(active.begin(), active.end(), 1);
        return;
    }
    rows.applyToSelected([&](auto row) {
        active[static_cast<size_t>(row)] = 1;
    });
}

inline OmniSubOpValueEncoding ClassifyEncoding(
    const facebook::velox::BaseVector& base) {
    if (base.isFlatEncoding()) {
        return OMNI_SUBOP_VALUE_FLAT;
    }
    if (base.encoding() == facebook::velox::VectorEncoding::Simple::CONSTANT) {
        return OMNI_SUBOP_VALUE_CONSTANT;
    }
    return OMNI_SUBOP_VALUE_DICTIONARY;
}

inline int32_t FillNulls(
    const facebook::velox::VectorPtr& arg,
    const facebook::velox::SelectivityVector& rows,
    std::vector<uint8_t>& nulls,
    OmniSubOpValueEncoding& encoding) {
    nulls.assign(static_cast<size_t>(rows.size()), 0);
    if (rows.size() == 0) {
        encoding = OMNI_SUBOP_VALUE_FLAT;
        return OMNI_SUBOP_COUNT_OK;
    }

    facebook::velox::LazyVector::ensureLoadedRows(arg, rows);
    const auto* base = arg->loadedVector();
    encoding = ClassifyEncoding(*base);

    if (encoding == OMNI_SUBOP_VALUE_CONSTANT) {
        if (base->isNullAt(0)) {
            rows.applyToSelected([&](auto row) {
                nulls[static_cast<size_t>(row)] = 1;
            });
        }
        return OMNI_SUBOP_COUNT_OK;
    }

    if (encoding == OMNI_SUBOP_VALUE_FLAT) {
        if (!base->mayHaveNulls()) {
            return OMNI_SUBOP_COUNT_OK;
        }
        rows.applyToSelected([&](auto row) {
            nulls[static_cast<size_t>(row)] = base->isNullAt(row) ? 1 : 0;
        });
        return OMNI_SUBOP_COUNT_OK;
    }

    facebook::velox::DecodedVector decoded(*arg, rows);
    if (!decoded.mayHaveNulls()) {
        return OMNI_SUBOP_COUNT_OK;
    }
    rows.applyToSelected([&](auto row) {
        nulls[static_cast<size_t>(row)] = decoded.isNullAt(row) ? 1 : 0;
    });
    return OMNI_SUBOP_COUNT_OK;
}

inline int32_t SubmitSingleState(
    int64_t* state,
    uint64_t selectedRows,
    uint64_t countedRows,
    uint64_t nullSkippedRows,
    int64_t delta,
    OmniSubOpCountResult* result) {
    OmniSubOpCountSingleStateArgs kernelArgs{};
    kernelArgs.abi_version = OMNI_SUBOP_ABI_VERSION;
    kernelArgs.state = state;
    kernelArgs.selected_rows = selectedRows;
    kernelArgs.counted_rows = countedRows;
    kernelArgs.null_skipped_rows = nullSkippedRows;
    kernelArgs.delta = delta;
    return omni_subop_count_update_single_state(&kernelArgs, result);
}

template <typename RowValueAt>
inline int32_t MergeSelectedSingleState(
    const facebook::velox::SelectivityVector& rows,
    int64_t* state,
    RowValueAt rowValueAt,
    OmniSubOpCountResult* result) {
    const uint64_t selectedRows = rows.countSelected();
    uint64_t countedRows = 0;
    uint64_t nullSkippedRows = 0;
    int64_t delta = 0;
    int32_t failure = OMNI_SUBOP_COUNT_OK;

    rows.applyToSelected([&](auto row) {
        if (failure != OMNI_SUBOP_COUNT_OK) {
            return;
        }
        int64_t value = 0;
        if (!rowValueAt(row, &value)) {
            ++nullSkippedRows;
            return;
        }
        if (value < 0) {
            failure = OMNI_SUBOP_COUNT_INVALID_STATE;
            return;
        }
        ++countedRows;
        if (delta > std::numeric_limits<int64_t>::max() - value) {
            failure = OMNI_SUBOP_COUNT_OVERFLOW;
            return;
        }
        delta += value;
    });

    if (failure != OMNI_SUBOP_COUNT_OK) {
        return failure;
    }
    return SubmitSingleState(
        state,
        selectedRows,
        countedRows,
        nullSkippedRows,
        delta,
        result);
}

} // namespace count_detail

/*
 * Count a single-group batch without materializing normalized row arrays.
 */
inline int32_t CountRawSingleGroup(
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int64_t* state,
    OmniSubOpCountResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() > 1) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    const uint64_t selectedRows = rows.countSelected();
    if (args.empty() || rows.size() == 0) {
        return count_detail::SubmitSingleState(
            state,
            selectedRows,
            selectedRows,
            0,
            static_cast<int64_t>(selectedRows),
            result);
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    const auto* base = args[0]->loadedVector();
    if (base->isFlatEncoding() && !base->mayHaveNulls()) {
        return count_detail::SubmitSingleState(
            state,
            selectedRows,
            selectedRows,
            0,
            static_cast<int64_t>(selectedRows),
            result);
    }
    if (base->encoding() == facebook::velox::VectorEncoding::Simple::CONSTANT) {
        const bool isNull = base->isNullAt(0);
        return count_detail::SubmitSingleState(
            state,
            selectedRows,
            isNull ? 0 : selectedRows,
            isNull ? selectedRows : 0,
            isNull ? 0 : static_cast<int64_t>(selectedRows),
            result);
    }

    uint64_t nullSkippedRows = 0;
    if (base->isFlatEncoding()) {
        rows.applyToSelected([&](auto row) {
            if (base->isNullAt(row)) {
                ++nullSkippedRows;
            }
        });
    } else {
        facebook::velox::DecodedVector decoded(*args[0], rows);
        rows.applyToSelected([&](auto row) {
            if (decoded.isNullAt(row)) {
                ++nullSkippedRows;
            }
        });
    }
    return count_detail::SubmitSingleState(
        state,
        selectedRows,
        selectedRows - nullSkippedRows,
        nullSkippedRows,
            static_cast<int64_t>(selectedRows - nullSkippedRows),
        result);
}

/*
 * Merge partial counts into a single group without normalized row arrays.
 * Flat inputs use their contiguous values directly; constant and dictionary
 * inputs retain their engine-native decoding rules.
 */
inline int32_t CountMergeSingleGroup(
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    int64_t* state,
    OmniSubOpCountResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() != 1) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (rows.size() == 0) {
        return count_detail::SubmitSingleState(state, 0, 0, 0, 0, result);
    }

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    const auto* base = args[0]->loadedVector();
    if (base->isFlatEncoding()) {
        const auto* flat =
            base->asUnchecked<facebook::velox::FlatVector<int64_t>>();
        const bool mayHaveNulls = base->mayHaveNulls();
        return count_detail::MergeSelectedSingleState(
            rows,
            state,
            [&](auto row, int64_t* value) {
                if (mayHaveNulls && base->isNullAt(row)) {
                    return false;
                }
                *value = flat->rawValues()[row];
                return true;
            },
            result);
    }
    if (base->encoding() == facebook::velox::VectorEncoding::Simple::CONSTANT) {
        const uint64_t selectedRows = rows.countSelected();
        if (base->isNullAt(0)) {
            return count_detail::SubmitSingleState(
                state, selectedRows, 0, selectedRows, 0, result);
        }
        const auto* constant =
            base->asUnchecked<facebook::velox::ConstantVector<int64_t>>();
        const int64_t value = constant->value();
        if (value < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
        const auto maxCount =
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        if (value != 0 && selectedRows > maxCount / static_cast<uint64_t>(value)) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
        return count_detail::SubmitSingleState(
            state,
            selectedRows,
            selectedRows,
            0,
            value * static_cast<int64_t>(selectedRows),
            result);
    }

    facebook::velox::DecodedVector decoded(*args[0], rows);
    return count_detail::MergeSelectedSingleState(
        rows,
        state,
        [&](auto row, int64_t* value) {
            if (decoded.isNullAt(row)) {
                return false;
            }
            *value = decoded.valueAt<int64_t>(row);
            return true;
        },
        result);
}

/*
 * Count raw input rows.
 *
 * StateAt must return the address of the int64_t count accumulator for a row.
 * The caller normally passes:
 *
 *   [&](facebook::velox::vector_size_t row) {
 *       return value<int64_t>(groups[row]);
 *   }
 */
template <typename StateAt>
int32_t CountRawBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    StateAt stateAt,
    OmniSubOpCountResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max()) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args.size() > 1) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    std::vector<uint8_t> active;
    count_detail::FillActive(rows, active);

    OmniSubOpValueEncoding encoding = OMNI_SUBOP_VALUE_FLAT;
    std::vector<uint8_t> nulls;
    const auto mode = args.empty() ? OMNI_SUBOP_COUNT_ALL : OMNI_SUBOP_COUNT_EXPR;
    if (mode == OMNI_SUBOP_COUNT_EXPR) {
        const auto status = count_detail::FillNulls(
            args[0], rows, nulls, encoding);
        if (status != OMNI_SUBOP_COUNT_OK) {
            return status;
        }
    }

    std::vector<int64_t*> rowCounts(static_cast<size_t>(rows.size()), nullptr);
    rows.applyToSelected([&](auto row) {
        rowCounts[static_cast<size_t>(row)] = stateAt(row);
    });

    OmniSubOpCountArgs kernelArgs{};
    kernelArgs.abi_version = OMNI_SUBOP_ABI_VERSION;
    kernelArgs.num_rows = static_cast<int32_t>(rows.size());
    kernelArgs.mode = mode;
    kernelArgs.encoding = encoding;
    kernelArgs.active_rows = active.empty() ? nullptr : active.data();
    kernelArgs.nulls = nulls.empty() ? nullptr : nulls.data();
    kernelArgs.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    (void)groups;
    return omni_subop_count_update_raw(&kernelArgs, result);
}

/*
 * Merge partial count rows. The adapter preserves the partial-value NULL mask;
 * the kernel skips NULL rows and only writes non-NULL partial counts.
 */
template <typename StateAt>
int32_t CountMergeBatch(
    char** groups,
    const facebook::velox::SelectivityVector& rows,
    const std::vector<facebook::velox::VectorPtr>& args,
    StateAt stateAt,
    OmniSubOpCountResult* result) {
    if (rows.size() > std::numeric_limits<int32_t>::max() || args.size() != 1) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    std::vector<uint8_t> active;
    count_detail::FillActive(rows, active);
    std::vector<int64_t> partial(static_cast<size_t>(rows.size()), 0);
    std::vector<uint8_t> partialNulls(static_cast<size_t>(rows.size()), 0);

    facebook::velox::LazyVector::ensureLoadedRows(args[0], rows);
    const auto* base = args[0]->loadedVector();
    if (base->isFlatEncoding()) {
        const auto* flat =
            base->asUnchecked<facebook::velox::FlatVector<int64_t>>();
        rows.applyToSelected([&](auto row) {
            if (base->isNullAt(row)) {
                partialNulls[static_cast<size_t>(row)] = 1;
                return;
            }
            partial[static_cast<size_t>(row)] = flat->rawValues()[row];
        });
    } else {
        facebook::velox::DecodedVector decoded(*args[0], rows);
        rows.applyToSelected([&](auto row) {
            if (decoded.isNullAt(row)) {
                partialNulls[static_cast<size_t>(row)] = 1;
                return;
            }
            partial[static_cast<size_t>(row)] = decoded.valueAt<int64_t>(row);
        });
    }

    std::vector<int64_t*> rowCounts(static_cast<size_t>(rows.size()), nullptr);
    rows.applyToSelected([&](auto row) {
        rowCounts[static_cast<size_t>(row)] = stateAt(row);
    });

    OmniSubOpCountMergeArgs kernelArgs{};
    kernelArgs.abi_version = OMNI_SUBOP_ABI_VERSION;
    kernelArgs.num_rows = static_cast<int32_t>(rows.size());
    kernelArgs.active_rows = active.empty() ? nullptr : active.data();
    kernelArgs.nulls = partialNulls.empty() ? nullptr : partialNulls.data();
    kernelArgs.partial_counts = partial.empty() ? nullptr : partial.data();
    kernelArgs.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    (void)groups;
    return omni_subop_count_merge_partial(&kernelArgs, result);
}

} // namespace omni::subop::velox

#endif /* OMNI_SUBOP_ADAPTER_VELOX_COUNT_BATCH_H */
