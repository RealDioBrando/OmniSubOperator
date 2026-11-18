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

#include <array>
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

/*
 * Accumulates deltas without allocating a map for the common single-state
 * case. The map is created lazily only when a second state pointer appears.
 *
 * No engine state is changed until Commit(), after every delta and destination
 * value has passed the overflow check.
 */
class DeltaAccumulator {
public:
    bool Add(int64_t* state, int64_t value) {
        if (state == nullptr || *state < 0 || value < 0) {
            return false;
        }
        if (value == 0) {
            return true;
        }

        if (!hasValue_) {
            singleState_ = state;
            singleDelta_ = value;
            hasValue_ = true;
            return true;
        }

        if (!usingMap_ && inlineUsed_ == 0 && state == singleState_) {
            if (singleDelta_ > kMaxCount - value) {
                return false;
            }
            singleDelta_ += value;
            return true;
        }

        if (!usingMap_) {
            if (inlineUsed_ == 0) {
                const auto slot = HashState(singleState_) & (kInlineSlots - 1);
                inlineEntries_[slot] = DeltaEntry{singleState_, singleDelta_, true};
                inlineUsed_ = 1;
            }

            const auto startSlot = HashState(state) & (kInlineSlots - 1);
            size_t slot = startSlot;
            while (true) {
                auto& entry = inlineEntries_[slot];
                if (!entry.used) {
                    if (inlineUsed_ >= kMaxInline) {
                        UpgradeToMap();
                        break;
                    }
                    entry = DeltaEntry{state, value, true};
                    ++inlineUsed_;
                    return true;
                }
                if (entry.state == state) {
                    if (entry.value > kMaxCount - value) {
                        return false;
                    }
                    entry.value += value;
                    return true;
                }
                slot = (slot + 1) & (kInlineSlots - 1);
                if (slot == startSlot) {
                    UpgradeToMap();
                    break;
                }
            }
        }

        auto it = delta_.find(state);
        if (it == delta_.end()) {
            delta_.emplace(state, value);
            return true;
        }
        if (it->second > kMaxCount - value) {
            return false;
        }
        it->second += value;
        return true;
    }

    int32_t Commit(
        OmniSubOpCountResult stats,
        OmniSubOpCountResult* result) {
        if (!hasValue_) {
            stats.abi_version = OMNI_SUBOP_ABI_VERSION;
            if (result != nullptr) {
                *result = stats;
            }
            return OMNI_SUBOP_COUNT_OK;
        }

        if (!usingMap_ && inlineUsed_ == 0) {
            if (*singleState_ > kMaxCount - singleDelta_) {
                return OMNI_SUBOP_COUNT_OVERFLOW;
            }
            *singleState_ += singleDelta_;
            stats.abi_version = OMNI_SUBOP_ABI_VERSION;
            stats.groups_touched = 1;
            if (result != nullptr) {
                *result = stats;
            }
            return OMNI_SUBOP_COUNT_OK;
        }

        if (!usingMap_) {
            for (const auto& entry : inlineEntries_) {
                if (entry.used && *entry.state > kMaxCount - entry.value) {
                    return OMNI_SUBOP_COUNT_OVERFLOW;
                }
            }
            for (const auto& entry : inlineEntries_) {
                if (entry.used) {
                    *entry.state += entry.value;
                }
            }
            stats.abi_version = OMNI_SUBOP_ABI_VERSION;
            stats.groups_touched = static_cast<uint32_t>(inlineUsed_);
            if (result != nullptr) {
                *result = stats;
            }
            return OMNI_SUBOP_COUNT_OK;
        }

        for (const auto& entry : delta_) {
            if (*entry.first > kMaxCount - entry.second) {
                return OMNI_SUBOP_COUNT_OVERFLOW;
            }
        }
        for (const auto& entry : delta_) {
            *entry.first += entry.second;
        }

        stats.abi_version = OMNI_SUBOP_ABI_VERSION;
        stats.groups_touched = static_cast<uint32_t>(delta_.size());
        if (result != nullptr) {
            *result = stats;
        }
        return OMNI_SUBOP_COUNT_OK;
    }

private:
    static constexpr size_t kInlineSlots = 128;
    static constexpr size_t kMaxInline = 64;

    struct DeltaEntry {
        int64_t* state;
        int64_t value;
        bool used;
    };

    static size_t HashState(int64_t* state) {
        const auto value = reinterpret_cast<uintptr_t>(state);
        return (value >> 3) ^ (value >> 13);
    }

    void UpgradeToMap() {
        delta_.reserve(inlineUsed_ + 8);
        for (const auto& entry : inlineEntries_) {
            if (entry.used) {
                delta_.emplace(entry.state, entry.value);
            }
        }
        usingMap_ = true;
    }

    std::array<DeltaEntry, kInlineSlots> inlineEntries_{};
    size_t inlineUsed_ = 0;
    std::unordered_map<int64_t*, int64_t> delta_;
    int64_t* singleState_ = nullptr;
    int64_t singleDelta_ = 0;
    bool hasValue_ = false;
    bool usingMap_ = false;
};

inline int64_t* ResolveGroupedState(uint8_t* base, uint32_t stateOffset) {
    if (base == nullptr) {
        return nullptr;
    }
    // Engines own group-row layout and offset alignment. Velox row containers
    // may place the accumulator at a non-8-byte-aligned group offset; the
    // engine's own value<T>(group) accessor uses the same address.
    return reinterpret_cast<int64_t*>(base + stateOffset);
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

    DeltaAccumulator delta;
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
        if (!delta.Add(args->row_counts[row], 1)) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return delta.Commit(stats, result);
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

    DeltaAccumulator delta;
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
        if (!delta.Add(args->row_counts[row], args->partial_counts[row])) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return delta.Commit(stats, result);
}

int32_t UpdateGroupedImpl(
    const OmniSubOpCountGroupedArgs* args,
    OmniSubOpCountResult* result) {
    if (args == nullptr || args->abi_version != OMNI_SUBOP_ABI_VERSION) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->num_rows < 0 || !IsSupportedMode(args->mode)) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->num_rows > 0 && args->row_bases == nullptr) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    // The production path does not request diagnostic statistics. For a
    // normal count state, prevalidate the state addresses and overflow margin,
    // then increment directly. This avoids a per-row hash-table delta update
    // for the common dense grouped case while preserving the no-partial-write
    // contract. If any state is close enough to the limit that the upper bound
    // cannot prove safety, use the general DeltaAccumulator below.
    if (result == nullptr) {
        bool canIncrementDirectly = true;
        for (int32_t row = 0; row < args->num_rows; ++row) {
            if (!IsActive(args->active_rows, row)) {
                continue;
            }
            int64_t* state = ResolveGroupedState(
                args->row_bases[row], args->state_offset);
            if (state == nullptr || *state < 0) {
                return OMNI_SUBOP_COUNT_INVALID_STATE;
            }
            if (args->mode == OMNI_SUBOP_COUNT_EXPR &&
                args->nulls != nullptr && args->nulls[row] != 0) {
                continue;
            }
            if (*state > kMaxCount - args->num_rows) {
                canIncrementDirectly = false;
                break;
            }
        }
        if (canIncrementDirectly) {
            for (int32_t row = 0; row < args->num_rows; ++row) {
                if (!IsActive(args->active_rows, row)) {
                    continue;
                }
                if (args->mode == OMNI_SUBOP_COUNT_EXPR &&
                    args->nulls != nullptr && args->nulls[row] != 0) {
                    continue;
                }
                ++*ResolveGroupedState(
                    args->row_bases[row], args->state_offset);
            }
            return OMNI_SUBOP_COUNT_OK;
        }
    }

    DeltaAccumulator delta;
    OmniSubOpCountResult stats{};

    for (int32_t row = 0; row < args->num_rows; ++row) {
        if (!IsActive(args->active_rows, row)) {
            continue;
        }
        ++stats.selected_rows;

        int64_t* state = ResolveGroupedState(args->row_bases[row], args->state_offset);
        if (state == nullptr || *state < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
        if (args->mode == OMNI_SUBOP_COUNT_EXPR &&
            args->nulls != nullptr && args->nulls[row] != 0) {
            ++stats.null_skipped_rows;
            continue;
        }
        ++stats.counted_rows;
        if (!delta.Add(state, 1)) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return delta.Commit(stats, result);
}

int32_t MergeGroupedImpl(
    const OmniSubOpCountGroupedMergeArgs* args,
    OmniSubOpCountResult* result) {
    if (args == nullptr || args->abi_version != OMNI_SUBOP_ABI_VERSION) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->num_rows < 0 ||
        (args->num_rows > 0 &&
         (args->row_bases == nullptr || args->partial_counts == nullptr))) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    DeltaAccumulator delta;
    OmniSubOpCountResult stats{};

    for (int32_t row = 0; row < args->num_rows; ++row) {
        if (!IsActive(args->active_rows, row)) {
            continue;
        }
        ++stats.selected_rows;

        int64_t* state = ResolveGroupedState(args->row_bases[row], args->state_offset);
        if (state == nullptr || *state < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
        if (args->nulls != nullptr && args->nulls[row] != 0) {
            ++stats.null_skipped_rows;
            continue;
        }
        if (args->partial_counts[row] < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
        ++stats.counted_rows;
        if (!delta.Add(state, args->partial_counts[row])) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
    }

    return delta.Commit(stats, result);
}

int32_t UpdateSingleStateImpl(
    const OmniSubOpCountSingleStateArgs* args,
    OmniSubOpCountResult* result) {
    if (args == nullptr || args->abi_version != OMNI_SUBOP_ABI_VERSION) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }
    if (args->delta < 0 || args->null_skipped_rows > args->selected_rows ||
        args->counted_rows != args->selected_rows - args->null_skipped_rows ||
        (args->counted_rows == 0 && args->delta != 0)) {
        return OMNI_SUBOP_COUNT_INVALID_ARGUMENT;
    }

    OmniSubOpCountResult stats{};
    stats.selected_rows = args->selected_rows;
    stats.counted_rows = args->counted_rows;
    stats.null_skipped_rows = args->null_skipped_rows;

    if (args->counted_rows != 0) {
        if (args->state == nullptr || *args->state < 0) {
            return OMNI_SUBOP_COUNT_INVALID_STATE;
        }
        if (*args->state > kMaxCount - args->delta) {
            return OMNI_SUBOP_COUNT_OVERFLOW;
        }
        *args->state += args->delta;
        stats.groups_touched = 1;
    }

    stats.abi_version = OMNI_SUBOP_ABI_VERSION;
    if (result != nullptr) {
        *result = stats;
    }
    return OMNI_SUBOP_COUNT_OK;
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

extern "C" int32_t omni_subop_count_update_single_state(
    const OmniSubOpCountSingleStateArgs* args,
    OmniSubOpCountResult* result) noexcept {
    ClearResult(result);
    try {
        return UpdateSingleStateImpl(args, result);
    } catch (...) {
        return OMNI_SUBOP_COUNT_INTERNAL_ERROR;
    }
}

extern "C" int32_t omni_subop_count_update_grouped(
    const OmniSubOpCountGroupedArgs* args,
    OmniSubOpCountResult* result) noexcept {
    ClearResult(result);
    try {
        return UpdateGroupedImpl(args, result);
    } catch (...) {
        return OMNI_SUBOP_COUNT_INTERNAL_ERROR;
    }
}

extern "C" int32_t omni_subop_count_merge_grouped(
    const OmniSubOpCountGroupedMergeArgs* args,
    OmniSubOpCountResult* result) noexcept {
    ClearResult(result);
    try {
        return MergeGroupedImpl(args, result);
    } catch (...) {
        return OMNI_SUBOP_COUNT_INTERNAL_ERROR;
    }
}
