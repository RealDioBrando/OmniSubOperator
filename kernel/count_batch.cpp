/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Phase 0 count implementation.
 *
 * The implementation deliberately builds a temporary pointer-to-delta map and
 * performs all validation and overflow checks before touching engine state.
 * This gives the adapter a clean no-partial-write fallback contract.
 */

#include "count_batch.h"

#include <cstring>
#include <limits>
#include <unordered_map>

namespace {

constexpr int64_t kMaxCount = std::numeric_limits<int64_t>::max();

bool IsSupportedEncoding(OmniSubOpValueEncoding encoding) {
    switch (encoding) {
        case OMNI_SUBOP_VALUE_FLAT:
        case OMNI_SUBOP_VALUE_CONSTANT:
        case OMNI_SUBOP_VALUE_DICTIONARY:
            return true;
    }
    return false;
}

bool IsSupportedMode(OmniSubOpCountMode mode) {
    return mode == OMNI_SUBOP_COUNT_ALL || mode == OMNI_SUBOP_COUNT_EXPR;
}

bool IsActive(const uint8_t* activeRows, int32_t row) {
    return activeRows == nullptr || activeRows[row] != 0;
}

void ClearResult(OmniSubOpCountResult* result) {
    if (result == nullptr) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->abi_version = OMNI_SUBOP_ABI_VERSION;
}

bool ValidateRowCounts(
    const uint8_t* activeRows,
    int64_t* const* rowCounts,
    int32_t numRows) {
    if (numRows > 0 && rowCounts == nullptr) {
        return false;
    }
    for (int32_t row = 0; row < numRows; ++row) {
        if (!IsActive(activeRows, row)) {
            continue;
        }
        int64_t* state = rowCounts[row];
        if (state == nullptr || *state < 0) {
            return false;
        }
    }
    return true;
}

bool AccumulateDelta(
    std::unordered_map<int64_t*, int64_t>& delta,
    int64_t* state,
    int64_t value) {
    if (state == nullptr || value < 0) {
        return false;
    }
    if (value == 0) {
        return true;
    }
    auto it = delta.find(state);
    if (it == delta.end()) {
        delta.emplace(state, value);
        return true;
    }
    if (it->second > kMaxCount - value) {
        return false;
    }
    it->second += value;
    return true;
}

int32_t CommitDelta(
    const std::unordered_map<int64_t*, int64_t>& delta,
    OmniSubOpCountResult stats,
    OmniSubOpCountResult* result) {
    /* Check every addition before the first state write. */
    for (const auto& entry : delta) {
        if (*entry.first > kMaxCount - entry.second) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    for (const auto& entry : delta) {
        *entry.first += entry.second;
    }

    stats.abi_version = OMNI_SUBOP_ABI_VERSION;
    stats.groups_touched = static_cast<uint32_t>(delta.size());
    if (result != nullptr) {
        *result = stats;
    }
    return OMNI_SUBOP_COUNT_OK;
}

int32_t UpdateRawImpl(
    const OmniSubOpCountArgs* args,
    OmniSubOpCountResult* result) {
    if (args == nullptr || args->abi_version != OMNI_SUBOP_ABI_VERSION) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->num_rows < 0 || !IsSupportedMode(args->mode)) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (!IsSupportedEncoding(args->encoding)) {
        return OMNI_SUBOP_COUNT_UNSUPPORTED_ENCODING;
    }
    if (!ValidateRowCounts(args->active_rows, args->row_counts, args->num_rows)) {
        return OMNI_SUBOP_COUNT_INVALID_STATE;
    }

    std::unordered_map<int64_t*, int64_t> delta;
    delta.reserve(static_cast<size_t>(args->num_rows));
    OmniSubOpCountResult stats{};

    for (int32_t row = 0; row < args->num_rows; ++row) {
        if (!IsActive(args->active_rows, row)) {
            continue;
        }
        ++stats.selected_rows;

        if (args->mode == OMNI_SUBOP_COUNT_EXPR && args->nulls != nullptr && args->nulls[row] != 0) {
            ++stats.null_skipped_rows;
            continue;
        }

        ++stats.counted_rows;
        if (!AccumulateDelta(delta, args->row_counts[row], 1)) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return CommitDelta(delta, stats, result);
}

int32_t MergeImpl(
    const OmniSubOpCountMergeArgs* args,
    OmniSubOpCountResult* result) {
    if (args == nullptr || args->abi_version != OMNI_SUBOP_ABI_VERSION) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->num_rows < 0 || (args->num_rows > 0 && args->partial_counts == nullptr)) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (!ValidateRowCounts(args->active_rows, args->row_counts, args->num_rows)) {
        return OMNI_SUBOP_COUNT_INVALID_STATE;
    }

    for (int32_t row = 0; row < args->num_rows; ++row) {
        if (!IsActive(args->active_rows, row)) {
            continue;
        }
        const bool isNull =
            args->nulls != nullptr && args->nulls[row] != 0;
        if (!isNull && args->partial_counts[row] < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
    }

    std::unordered_map<int64_t*, int64_t> delta;
    delta.reserve(static_cast<size_t>(args->num_rows));
    OmniSubOpCountResult stats{};

    for (int32_t row = 0; row < args->num_rows; ++row) {
        if (!IsActive(args->active_rows, row)) {
            continue;
        }
        ++stats.selected_rows;
        if (args->nulls != nullptr && args->nulls[row] != 0) {
            ++stats.null_skipped_rows;
            continue;
        }
        ++stats.counted_rows;
        if (!AccumulateDelta(delta, args->row_counts[row], args->partial_counts[row])) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return CommitDelta(delta, stats, result);
}

} // namespace

extern "C" int32_t omni_subop_count_abi_version(void) noexcept {
    return static_cast<int32_t>(OMNI_SUBOP_ABI_VERSION);
}

extern "C" int32_t omni_subop_count_update_raw(
    const OmniSubOpCountArgs* args,
    OmniSubOpCountResult* result) noexcept {
    ClearResult(result);
    try {
        return UpdateRawImpl(args, result);
    } catch (...) {
        return OMNI_SUBOP_COUNT_INTERNAL_ERROR;
    }
}

extern "C" int32_t omni_subop_count_merge_partial(
    const OmniSubOpCountMergeArgs* args,
    OmniSubOpCountResult* result) noexcept {
    ClearResult(result);
    try {
        return MergeImpl(args, result);
    } catch (...) {
        return OMNI_SUBOP_COUNT_INTERNAL_ERROR;
    }
}
