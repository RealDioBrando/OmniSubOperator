/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Dependency-free Phase 0 tests for the count micro-operator.
 */

#include "kernel/count_batch.h"

#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

int gChecks = 0;
int gFailures = 0;

#define CHECK(condition)                                                        \
    do {                                                                       \
        ++gChecks;                                                             \
        if (!(condition)) {                                                    \
            ++gFailures;                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);   \
        }                                                                      \
    } while (0)

#define CHECK_EQ(actual, expected)                                             \
    do {                                                                       \
        ++gChecks;                                                             \
        const auto actualValue = (actual);                                     \
        const auto expectedValue = (expected);                                 \
        if (!(actualValue == expectedValue)) {                                 \
            ++gFailures;                                                       \
            std::printf(                                                       \
                "FAIL %s:%d: %s != %s\n",                                     \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #actual,                                                       \
                #expected);                                                    \
        }                                                                      \
    } while (0)

OmniSubOpCountArgs MakeRawArgs(
    std::vector<int64_t*>& rowCounts,
    std::vector<uint8_t>& active,
    std::vector<uint8_t>* nulls,
    OmniSubOpCountMode mode = OMNI_SUBOP_COUNT_ALL,
    OmniSubOpValueEncoding encoding = OMNI_SUBOP_VALUE_FLAT) {
    OmniSubOpCountArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(rowCounts.size());
    args.mode = mode;
    args.encoding = encoding;
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls == nullptr || nulls->empty() ? nullptr : nulls->data();
    args.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    return args;
}

OmniSubOpCountMergeArgs MakeMergeArgs(
    std::vector<int64_t*>& rowCounts,
    std::vector<uint8_t>& active,
    std::vector<int64_t>& partial,
    std::vector<uint8_t>* nulls = nullptr) {
    OmniSubOpCountMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(rowCounts.size());
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls == nullptr || nulls->empty() ? nullptr : nulls->data();
    args.partial_counts = partial.empty() ? nullptr : partial.data();
    args.row_counts = rowCounts.empty() ? nullptr : rowCounts.data();
    return args;
}

OmniSubOpCountGroupedArgs MakeGroupedArgs(
    std::vector<uint8_t*>& rowBases,
    std::vector<uint8_t>& active,
    std::vector<uint8_t>* nulls,
    OmniSubOpCountMode mode = OMNI_SUBOP_COUNT_ALL,
    uint32_t stateOffset = 0) {
    OmniSubOpCountGroupedArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(rowBases.size());
    args.mode = mode;
    args.state_offset = stateOffset;
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls == nullptr || nulls->empty() ? nullptr : nulls->data();
    args.row_bases = rowBases.empty() ? nullptr : rowBases.data();
    return args;
}

OmniSubOpCountGroupedMergeArgs MakeGroupedMergeArgs(
    std::vector<uint8_t*>& rowBases,
    std::vector<uint8_t>& active,
    std::vector<int64_t>& partial,
    std::vector<uint8_t>* nulls = nullptr,
    uint32_t stateOffset = 0) {
    OmniSubOpCountGroupedMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = static_cast<int32_t>(rowBases.size());
    args.state_offset = stateOffset;
    args.active_rows = active.empty() ? nullptr : active.data();
    args.nulls = nulls == nullptr || nulls->empty() ? nullptr : nulls->data();
    args.partial_counts = partial.empty() ? nullptr : partial.data();
    args.row_bases = rowBases.empty() ? nullptr : rowBases.data();
    return args;
}

inline std::vector<uint8_t*> GroupBases(
    std::vector<int64_t>& states,
    const std::vector<int32_t>& groups) {
    std::vector<uint8_t*> bases;
    bases.reserve(groups.size());
    for (auto group : groups) {
        bases.push_back(reinterpret_cast<uint8_t*>(&states[static_cast<size_t>(group)]));
    }
    return bases;
}

OmniSubOpCountSingleStateArgs MakeSingleStateArgs(
    int64_t* state,
    uint64_t selected,
    uint64_t counted,
    uint64_t nullSkipped,
    int64_t delta) {
    OmniSubOpCountSingleStateArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.state = state;
    args.selected_rows = selected;
    args.counted_rows = counted;
    args.null_skipped_rows = nullSkipped;
    args.delta = delta;
    return args;
}

void TestCountAllByGroup() {
    std::vector<int64_t> counts{10, 20, 30};
    std::vector<int64_t*> rowCounts{
        &counts[0], &counts[1], &counts[0], &counts[2], &counts[1]};
    std::vector<uint8_t> active{1, 1, 1, 1, 1};
    auto args = MakeRawArgs(rowCounts, active, nullptr);
    OmniSubOpCountResult result{};

    const auto status = omni_subop_count_update_raw(&args, &result);
    CHECK_EQ(status, OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 12);
    CHECK_EQ(counts[1], 22);
    CHECK_EQ(counts[2], 31);
    CHECK_EQ(result.selected_rows, 5u);
    CHECK_EQ(result.counted_rows, 5u);
    CHECK_EQ(result.null_skipped_rows, 0u);
    CHECK_EQ(result.groups_touched, 3u);
}

void TestCountExprNulls() {
    std::vector<int64_t> counts{0, 0};
    std::vector<int64_t*> rowCounts{
        &counts[0], &counts[0], &counts[0], &counts[1], &counts[1]};
    std::vector<uint8_t> active{1, 1, 1, 1, 1};
    std::vector<uint8_t> nulls{0, 1, 0, 0, 1};
    auto args = MakeRawArgs(
        rowCounts, active, &nulls, OMNI_SUBOP_COUNT_EXPR);
    OmniSubOpCountResult result{};

    const auto status = omni_subop_count_update_raw(&args, &result);
    CHECK_EQ(status, OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 2);
    CHECK_EQ(counts[1], 1);
    CHECK_EQ(result.selected_rows, 5u);
    CHECK_EQ(result.counted_rows, 3u);
    CHECK_EQ(result.null_skipped_rows, 2u);
    CHECK_EQ(result.groups_touched, 2u);
}

void TestCountAllIgnoresNulls() {
    std::vector<int64_t> counts{0};
    std::vector<int64_t*> rowCounts{&counts[0], &counts[0]};
    std::vector<uint8_t> active{1, 1};
    std::vector<uint8_t> nulls{1, 1};
    auto args = MakeRawArgs(rowCounts, active, &nulls);
    OmniSubOpCountResult result{};

    CHECK_EQ(omni_subop_count_update_raw(&args, &result), OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 2);
    CHECK_EQ(result.counted_rows, 2u);
    CHECK_EQ(result.null_skipped_rows, 0u);
}

void TestSelectedRowsAndEncodings() {
    for (const auto encoding : {
             OMNI_SUBOP_VALUE_FLAT,
             OMNI_SUBOP_VALUE_CONSTANT,
             OMNI_SUBOP_VALUE_DICTIONARY}) {
        std::vector<int64_t> counts{0, 0};
        std::vector<int64_t*> rowCounts{
            nullptr, &counts[0], nullptr, &counts[1], &counts[0]};
        std::vector<uint8_t> active{0, 1, 0, 1, 1};
        std::vector<uint8_t> nulls{1, 0, 1, 0, 0};
        auto args = MakeRawArgs(
            rowCounts,
            active,
            &nulls,
            OMNI_SUBOP_COUNT_EXPR,
            encoding);
        OmniSubOpCountResult result{};

        CHECK_EQ(omni_subop_count_update_raw(&args, &result), OMNI_SUBOP_COUNT_OK);
        CHECK_EQ(counts[0], 2);
        CHECK_EQ(counts[1], 1);
        CHECK_EQ(result.selected_rows, 3u);
        CHECK_EQ(result.counted_rows, 3u);
        CHECK_EQ(result.null_skipped_rows, 0u);
    }
}

void TestMergePartial() {
    std::vector<int64_t> counts{5, 7, 9};
    std::vector<int64_t*> rowCounts{
        &counts[0], &counts[1], &counts[0], &counts[2]};
    std::vector<uint8_t> active{1, 1, 1, 1};
    std::vector<int64_t> partial{2, 3, 4, 5};
    auto args = MakeMergeArgs(rowCounts, active, partial);
    OmniSubOpCountResult result{};

    CHECK_EQ(omni_subop_count_merge_partial(&args, &result), OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 11);
    CHECK_EQ(counts[1], 10);
    CHECK_EQ(counts[2], 14);
    CHECK_EQ(result.selected_rows, 4u);
    CHECK_EQ(result.counted_rows, 4u);
    CHECK_EQ(result.groups_touched, 3u);
}

void TestMergeNullPartial() {
    std::vector<int64_t> counts{5, 7};
    std::vector<int64_t*> rowCounts{&counts[0], &counts[1], &counts[0]};
    std::vector<uint8_t> active{1, 1, 1};
    std::vector<int64_t> partial{2, 3, 4};
    std::vector<uint8_t> nulls{0, 1, 0};
    auto args = MakeMergeArgs(rowCounts, active, partial, &nulls);
    OmniSubOpCountResult result{};

    CHECK_EQ(omni_subop_count_merge_partial(&args, &result), OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 11);
    CHECK_EQ(counts[1], 7);
    CHECK_EQ(result.selected_rows, 3u);
    CHECK_EQ(result.counted_rows, 2u);
    CHECK_EQ(result.null_skipped_rows, 1u);
}

void TestInvalidStatePointerLeavesState() {
    std::vector<int64_t> counts{42, 43};
    std::vector<int64_t*> rowCounts{&counts[0], nullptr};
    std::vector<uint8_t> active{1, 1};
    auto args = MakeRawArgs(rowCounts, active, nullptr);
    OmniSubOpCountResult result{};

    CHECK_EQ(
        omni_subop_count_update_raw(&args, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(counts[0], 42);
    CHECK_EQ(counts[1], 43);
    CHECK_EQ(result.counted_rows, 0u);
}

void TestInvalidMergeLeavesState() {
    std::vector<int64_t> counts{42};
    std::vector<int64_t*> rowCounts{&counts[0]};
    std::vector<uint8_t> active{1};
    std::vector<int64_t> partial{-1};
    auto args = MakeMergeArgs(rowCounts, active, partial);
    OmniSubOpCountResult result{};

    CHECK_EQ(
        omni_subop_count_merge_partial(&args, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(counts[0], 42);
}

void TestOverflowLeavesState() {
    std::vector<int64_t> counts{std::numeric_limits<int64_t>::max()};
    std::vector<int64_t*> rowCounts{&counts[0]};
    std::vector<uint8_t> active{1};
    auto args = MakeRawArgs(rowCounts, active, nullptr);
    OmniSubOpCountResult result{};

    CHECK_EQ(
        omni_subop_count_update_raw(&args, &result),
        OMNI_SUBOP_COUNT_OVERFLOW);
    CHECK_EQ(counts[0], std::numeric_limits<int64_t>::max());
}

void TestSingleStateFastPath() {
    constexpr int32_t kRows = 1000;
    std::vector<int64_t> counts{5};
    std::vector<int64_t*> rowCounts(kRows, &counts[0]);
    std::vector<uint8_t> active(kRows, 1);
    std::vector<uint8_t> nulls(kRows, 0);
    for (int32_t row = 0; row < kRows; row += 10) {
        nulls[row] = 1;
    }

    auto rawArgs = MakeRawArgs(
        rowCounts, active, &nulls, OMNI_SUBOP_COUNT_EXPR);
    OmniSubOpCountResult rawResult{};
    CHECK_EQ(
        omni_subop_count_update_raw(&rawArgs, &rawResult),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 905);
    CHECK_EQ(rawResult.selected_rows, 1000u);
    CHECK_EQ(rawResult.counted_rows, 900u);
    CHECK_EQ(rawResult.null_skipped_rows, 100u);
    CHECK_EQ(rawResult.groups_touched, 1u);

    std::vector<int64_t> partial(kRows, 1);
    auto mergeArgs = MakeMergeArgs(rowCounts, active, partial, &nulls);
    OmniSubOpCountResult mergeResult{};
    CHECK_EQ(
        omni_subop_count_merge_partial(&mergeArgs, &mergeResult),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(counts[0], 1805);
    CHECK_EQ(mergeResult.selected_rows, 1000u);
    CHECK_EQ(mergeResult.counted_rows, 900u);
    CHECK_EQ(mergeResult.null_skipped_rows, 100u);
    CHECK_EQ(mergeResult.groups_touched, 1u);
}

void TestPreAggregatedSingleState() {
    int64_t state = 7;
    auto args = MakeSingleStateArgs(&state, 10, 7, 3, 7);
    OmniSubOpCountResult result{};

    CHECK_EQ(
        omni_subop_count_update_single_state(&args, &result),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(state, 14);
    CHECK_EQ(result.selected_rows, 10u);
    CHECK_EQ(result.counted_rows, 7u);
    CHECK_EQ(result.null_skipped_rows, 3u);
    CHECK_EQ(result.groups_touched, 1u);

    auto allNullArgs = MakeSingleStateArgs(nullptr, 4, 0, 4, 0);
    OmniSubOpCountResult allNullResult{};
    CHECK_EQ(
        omni_subop_count_update_single_state(&allNullArgs, &allNullResult),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(allNullResult.selected_rows, 4u);
    CHECK_EQ(allNullResult.counted_rows, 0u);
    CHECK_EQ(allNullResult.null_skipped_rows, 4u);
    CHECK_EQ(allNullResult.groups_touched, 0u);
}

void TestPreAggregatedSingleStateErrorsLeaveState() {
    int64_t state = std::numeric_limits<int64_t>::max();
    auto overflowArgs = MakeSingleStateArgs(&state, 1, 1, 0, 1);
    OmniSubOpCountResult result{};

    CHECK_EQ(
        omni_subop_count_update_single_state(&overflowArgs, &result),
        OMNI_SUBOP_COUNT_OVERFLOW);
    CHECK_EQ(state, std::numeric_limits<int64_t>::max());

    auto inconsistent = MakeSingleStateArgs(&state, 3, 2, 0, 1);
    CHECK_EQ(
        omni_subop_count_update_single_state(&inconsistent, &result),
        OMNI_SUBOP_COUNT_INVALID_ARGUMENT);
    CHECK_EQ(state, std::numeric_limits<int64_t>::max());

    auto negativeDelta = MakeSingleStateArgs(&state, 1, 1, 0, -1);
    CHECK_EQ(
        omni_subop_count_update_single_state(&negativeDelta, &result),
        OMNI_SUBOP_COUNT_INVALID_ARGUMENT);
    CHECK_EQ(state, std::numeric_limits<int64_t>::max());

    auto nullState = MakeSingleStateArgs(nullptr, 1, 1, 0, 1);
    CHECK_EQ(
        omni_subop_count_update_single_state(&nullState, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);

    int64_t negativeState = -1;
    auto invalidState = MakeSingleStateArgs(&negativeState, 1, 1, 0, 1);
    CHECK_EQ(
        omni_subop_count_update_single_state(&invalidState, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(negativeState, -1);
}

void TestGroupedStateBaseRawAndMerge() {
    std::vector<int64_t> states{10, 20, 30};
    const std::vector<int32_t> groups{0, 1, 0, 2, 1};
    auto rowBases = GroupBases(states, groups);
    std::vector<uint8_t> active;
    auto rawArgs = MakeGroupedArgs(rowBases, active, nullptr);
    OmniSubOpCountResult rawResult{};
    CHECK_EQ(
        omni_subop_count_update_grouped(&rawArgs, &rawResult),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(states[0], 12);
    CHECK_EQ(states[1], 22);
    CHECK_EQ(states[2], 31);
    CHECK_EQ(rawResult.selected_rows, 5u);
    CHECK_EQ(rawResult.groups_touched, 3u);

    std::vector<int64_t> mergeStates{0, 0, 0};
    auto mergeBases = GroupBases(mergeStates, groups);
    std::vector<int64_t> partial{1, 2, 3, 4, 5};
    std::vector<uint8_t> partialNulls{0, 1, 0, 0, 0};
    auto mergeArgs = MakeGroupedMergeArgs(
        mergeBases, active, partial, &partialNulls);
    OmniSubOpCountResult mergeResult{};
    CHECK_EQ(
        omni_subop_count_merge_grouped(&mergeArgs, &mergeResult),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(mergeStates[0], 4);
    CHECK_EQ(mergeStates[1], 5);
    CHECK_EQ(mergeStates[2], 4);
    CHECK_EQ(mergeResult.selected_rows, 5u);
    CHECK_EQ(mergeResult.counted_rows, 4u);
    CHECK_EQ(mergeResult.null_skipped_rows, 1u);
}

void TestGroupedStateBaseErrorsLeaveState() {
    std::vector<int64_t> states{7, -1};
    const std::vector<int32_t> groups{0, 1};
    auto rowBases = GroupBases(states, groups);
    std::vector<uint8_t> active;
    auto invalidStateArgs = MakeGroupedArgs(rowBases, active, nullptr);
    OmniSubOpCountResult result{};
    CHECK_EQ(
        omni_subop_count_update_grouped(&invalidStateArgs, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(states[0], 7);
    CHECK_EQ(states[1], -1);

    std::vector<int64_t> overflowState{std::numeric_limits<int64_t>::max()};
    std::vector<uint8_t*> overflowBases{
        reinterpret_cast<uint8_t*>(&overflowState[0])};
    auto overflowArgs = MakeGroupedArgs(overflowBases, active, nullptr);
    CHECK_EQ(
        omni_subop_count_update_grouped(&overflowArgs, &result),
        OMNI_SUBOP_COUNT_OVERFLOW);
    CHECK_EQ(overflowState[0], std::numeric_limits<int64_t>::max());
}

void TestEmptyBatchAndVersionChecks() {
    std::vector<int64_t*> rowCounts;
    OmniSubOpCountArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = 0;
    args.mode = OMNI_SUBOP_COUNT_ALL;
    args.encoding = OMNI_SUBOP_VALUE_FLAT;
    args.row_counts = nullptr;
    OmniSubOpCountResult result{};

    CHECK_EQ(omni_subop_count_update_raw(&args, &result), OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(result.selected_rows, 0u);
    CHECK_EQ(result.groups_touched, 0u);

    args.abi_version = 0;
    CHECK_EQ(
        omni_subop_count_update_raw(&args, &result),
        OMNI_SUBOP_COUNT_INVALID_ARGUMENT);

    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.encoding = static_cast<OmniSubOpValueEncoding>(99);
    CHECK_EQ(
        omni_subop_count_update_raw(&args, &result),
        OMNI_SUBOP_COUNT_UNSUPPORTED_ENCODING);
}

void TestAbiVersion() {
    CHECK_EQ(
        omni_subop_count_abi_version(),
        static_cast<int32_t>(OMNI_SUBOP_ABI_VERSION));
}

} // namespace

int main() {
    TestCountAllByGroup();
    TestCountExprNulls();
    TestCountAllIgnoresNulls();
    TestSelectedRowsAndEncodings();
    TestMergePartial();
    TestMergeNullPartial();
    TestSingleStateFastPath();
    TestPreAggregatedSingleState();
    TestPreAggregatedSingleStateErrorsLeaveState();
    TestGroupedStateBaseRawAndMerge();
    TestGroupedStateBaseErrorsLeaveState();
    TestInvalidStatePointerLeavesState();
    TestInvalidMergeLeavesState();
    TestOverflowLeavesState();
    TestEmptyBatchAndVersionChecks();
    TestAbiVersion();

    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
