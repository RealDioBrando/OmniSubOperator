#include "kernel/sum_batch.h"

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
            std::printf("FAIL %s:%d: %s != %s\n",                             \
                __FILE__, __LINE__, #actual, #expected);                       \
        }                                                                      \
    } while (0)

void TestSingleGroup() {
    std::vector<int64_t> values{5, 0, 0, -2};
    std::vector<uint8_t> nulls{0, 1, 0, 0};
    int64_t state = 0;
    bool initialized = false;
    omni::subop::SumResult result{};

    const auto status = omni::subop::SumRawSingleGroup(
        4,
        [&](int32_t row) { return values[static_cast<size_t>(row)]; },
        [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
        &state,
        initialized,
        &initialized,
        &result);
    CHECK_EQ(status, omni::subop::SUM_OK);
    CHECK_EQ(state, 3);
    CHECK(initialized);
    CHECK_EQ(result.selected_rows, 4u);
    CHECK_EQ(result.counted_rows, 3u);
    CHECK_EQ(result.null_skipped_rows, 1u);

    initialized = true;
    state = 10;
    CHECK_EQ(
        omni::subop::SumRawSingleGroup(
            4,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
            &state,
            initialized,
            &initialized,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(state, 13);
}

void TestGroupedRawAndMerge() {
    std::vector<int64_t> values{5, 7, -2, 10, 0};
    std::vector<uint8_t> nulls{0, 0, 0, 1, 0};
    std::vector<int64_t> states{10, 20};
    std::vector<bool> initialized{true, false};
    std::vector<int32_t> groups{0, 1, 0, 1, 1};
    std::vector<uint8_t*> bases{
        reinterpret_cast<uint8_t*>(&states[0]),
        reinterpret_cast<uint8_t*>(&states[1]),
        reinterpret_cast<uint8_t*>(&states[0]),
        reinterpret_cast<uint8_t*>(&states[1]),
        reinterpret_cast<uint8_t*>(&states[1])};

    omni::subop::SumResult result{};
    const auto status = omni::subop::SumRawGrouped(
        5,
        [&](int32_t row) { return values[static_cast<size_t>(row)]; },
        [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
        [&](int32_t row) -> int64_t* {
            return reinterpret_cast<int64_t*>(bases[static_cast<size_t>(row)]);
        },
        [&](int32_t row) { return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])]; },
        [&](int32_t row) {
            initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
        },
        &result);
    CHECK_EQ(status, omni::subop::SUM_OK);
    CHECK_EQ(states[0], 13);
    CHECK_EQ(states[1], 7);
    CHECK(initialized[0]);
    CHECK(initialized[1]);
    CHECK_EQ(result.selected_rows, 5u);
    CHECK_EQ(result.counted_rows, 4u);
    CHECK_EQ(result.null_skipped_rows, 1u);
    CHECK_EQ(result.groups_touched, 2u);

    states = {0, -5};
    initialized = {false, true};
    std::vector<int64_t> partial{8, 9, 11, 0, 0};
    std::vector<uint8_t> partialNulls{0, 1, 0, 0, 0};
    CHECK_EQ(
        omni::subop::SumMergeGrouped(
            5,
            [&](int32_t row) { return partial[static_cast<size_t>(row)]; },
            [&](int32_t row) { return partialNulls[static_cast<size_t>(row)] != 0; },
            [&](int32_t row) -> int64_t* {
                return reinterpret_cast<int64_t*>(bases[static_cast<size_t>(row)]);
            },
            [&](int32_t row) { return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])]; },
            [&](int32_t row) {
                initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
            },
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0], 19);
    CHECK_EQ(states[1], -5);
}

void TestWrapAndInvalidState() {
    int64_t state = std::numeric_limits<int64_t>::max();
    bool initialized = true;
    const int64_t min = std::numeric_limits<int64_t>::min();
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::SumRawSingleGroup(
            1,
            [&](int32_t) { return int64_t(1); },
            [&](int32_t) { return false; },
            &state,
            initialized,
            &initialized,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(state, min);

    state = 0;
    initialized = false;
    CHECK_EQ(
        omni::subop::SumRawGrouped(
            2,
            [&](int32_t row) { return row == 0 ? int64_t(1) : int64_t(2); },
            [&](int32_t) { return false; },
            [&](int32_t row) -> int64_t* {
                return row == 0 ? &state : nullptr;
            },
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            &result),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 0);
    CHECK(!initialized);
}

void TestPackedScalarSveAndDuplicates() {
    constexpr int32_t rows = 70;
    std::vector<int64_t> values(static_cast<size_t>(rows));
    std::vector<int32_t> groups(static_cast<size_t>(rows));
    std::vector<uint64_t> active((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<uint64_t> nulls((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<int64_t> initialState{10, -5, 0};
    std::vector<int64_t> states = initialState;
    std::vector<bool> initialized{true, false, true};
    std::vector<int64_t*> statePointers(static_cast<size_t>(rows), nullptr);

    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        values[i] = static_cast<int64_t>(row - 20);
        groups[i] = row % 3;
        if (row % 7 != 0) {
            active[i / 64] |= 1ULL << (i % 64);
        }
        if (row % 11 == 0) {
            nulls[i / 64] |= 1ULL << (i % 64);
        }
        if ((active[i / 64] & (1ULL << (i % 64))) != 0 &&
            (nulls[i / 64] & (1ULL << (i % 64))) == 0) {
            statePointers[i] = &states[static_cast<size_t>(groups[i])];
        }
    }

    std::vector<int64_t> expectedStates = initialState;
    std::vector<bool> expectedInitialized = initialized;
    uint64_t expectedSelected = 0;
    uint64_t expectedCounted = 0;
    uint64_t expectedNull = 0;
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        if ((active[i / 64] & (1ULL << (i % 64))) == 0) {
            continue;
        }
        ++expectedSelected;
        if ((nulls[i / 64] & (1ULL << (i % 64))) != 0) {
            ++expectedNull;
            continue;
        }
        ++expectedCounted;
        const auto group = static_cast<size_t>(groups[i]);
        if (expectedInitialized[group]) {
            expectedStates[group] = omni::subop::sum_detail::WrapAdd(
                expectedStates[group], values[i]);
        } else {
            expectedStates[group] = values[i];
        }
        expectedInitialized[group] = true;
    }

    omni::subop::SumInt64BatchView view;
    view.num_rows = rows;
    view.row_begin = 0;
    view.row_end = rows;
    view.value_count = rows;
    view.value_layout = omni::subop::SUM_VALUE_FLAT;
    view.values = values.data();
    view.state_values = statePointers.data();
    view.active_mask = active.data();
    view.null_mask = nulls.data();
    view.track_groups = true;

    omni::subop::SumResult result{};
    auto initializedAt = [&](int32_t row) {
        return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto markAt = [&](int32_t row) {
        initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
    };

    CHECK_EQ(
        omni::subop::SumRawPacked(
            view,
            initializedAt,
            markAt,
            omni::subop::SUM_EXECUTION_SCALAR,
            &result),
        omni::subop::SUM_OK);
    CHECK(states == expectedStates);
    CHECK(initialized == expectedInitialized);
    CHECK_EQ(result.selected_rows, expectedSelected);
    CHECK_EQ(result.counted_rows, expectedCounted);
    CHECK_EQ(result.null_skipped_rows, expectedNull);
    CHECK_EQ(result.groups_touched, 3u);

    states = initialState;
    initialized = {true, false, true};
    result = {};
    const auto sveStatus = omni::subop::SumRawPacked(
        view,
        initializedAt,
        markAt,
        omni::subop::SUM_EXECUTION_SVE,
        &result);
#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        CHECK_EQ(sveStatus, omni::subop::SUM_OK);
        CHECK(states == expectedStates);
        CHECK(initialized == expectedInitialized);
    } else {
        CHECK_EQ(sveStatus, omni::subop::SUM_UNSUPPORTED);
        CHECK(states == initialState);
    }
#else
    CHECK_EQ(sveStatus, omni::subop::SUM_UNSUPPORTED);
    CHECK(states == initialState);
#endif

    int64_t duplicateState = 0;
    bool duplicateInitialized = false;
    std::vector<int64_t> duplicateValues{0, 5, -2, 7};
    std::vector<int64_t*> duplicatePointers(4, &duplicateState);
    omni::subop::SumInt64BatchView duplicateView;
    duplicateView.num_rows = 4;
    duplicateView.row_begin = 0;
    duplicateView.row_end = 4;
    duplicateView.value_count = 4;
    duplicateView.value_layout = omni::subop::SUM_VALUE_FLAT;
    duplicateView.values = duplicateValues.data();
    duplicateView.state_values = duplicatePointers.data();
    auto duplicateInitializedAt = [&](int32_t) { return duplicateInitialized; };
    auto duplicateMarkAt = [&](int32_t) { duplicateInitialized = true; };
    CHECK_EQ(
        omni::subop::SumRawPacked(
            duplicateView,
            duplicateInitializedAt,
            duplicateMarkAt,
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(duplicateState, 10);
    CHECK(duplicateInitialized);
}

void TestPackedDenseUnroll() {
    std::vector<int64_t> values{1, 2, 3, 4, 5};
    int64_t state = 0;
    bool initialized = false;
    std::vector<int64_t*> pointers(values.size(), &state);
    omni::subop::SumInt64BatchView view;
    view.num_rows = static_cast<int32_t>(values.size());
    view.row_begin = 0;
    view.row_end = view.num_rows;
    view.value_count = view.num_rows;
    view.values = values.data();
    view.state_values = pointers.data();
    CHECK_EQ(
        omni::subop::SumRawPacked(
            view,
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(state, 15);
    CHECK(initialized);
}

void TestPackedDictionaryAndConstant() {
    std::vector<int64_t> dictionaryValues{2, 3, 5, 7};
    std::vector<int32_t> indices{0, 1, 0, 2, 3};
    std::vector<uint64_t> nulls{1};
    std::vector<int64_t> states{1, 2};
    std::vector<bool> initialized{false, true};
    std::vector<int64_t*> statePointers{
        &states[0], &states[1], &states[0], &states[1], &states[0]};

    omni::subop::SumInt64BatchView view;
    view.num_rows = 5;
    view.row_begin = 0;
    view.row_end = 5;
    view.value_count = 4;
    view.index_count = 5;
    view.value_layout = omni::subop::SUM_VALUE_DICTIONARY;
    view.values = dictionaryValues.data();
    view.value_indices = indices.data();
    view.state_values = statePointers.data();
    view.null_mask = nulls.data();
    view.track_groups = true;
    auto initializedAt = [&](int32_t row) {
        return initialized[static_cast<size_t>(row % 2)];
    };
    auto markAt = [&](int32_t row) {
        initialized[static_cast<size_t>(row % 2)] = true;
    };
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::SumRawPacked(
            view,
            initializedAt,
            markAt,
            omni::subop::SUM_EXECUTION_SCALAR,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0], 9);
    CHECK_EQ(states[1], 10);
    CHECK_EQ(result.counted_rows, 4u);
    CHECK_EQ(result.null_skipped_rows, 1u);
    CHECK_EQ(result.groups_touched, 2u);

    int64_t constantState = 0;
    bool constantInitialized = false;
    std::vector<int64_t*> constantPointers(3, &constantState);
    const int64_t constantValue = 9;
    omni::subop::SumInt64BatchView constantView;
    constantView.num_rows = 3;
    constantView.row_begin = 0;
    constantView.row_end = 3;
    constantView.value_count = 1;
    constantView.value_layout = omni::subop::SUM_VALUE_CONSTANT;
    constantView.values = &constantValue;
    constantView.state_values = constantPointers.data();
    CHECK_EQ(
        omni::subop::SumRawPacked(
            constantView,
            [&](int32_t) { return constantInitialized; },
            [&](int32_t) { constantInitialized = true; },
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(constantState, 27);
    CHECK(constantInitialized);
}

void TestPackedInvalidStateHasNoWrite() {
    int64_t state = 41;
    std::vector<int64_t> values{1, 2};
    std::vector<int64_t*> statePointers{&state, nullptr};
    omni::subop::SumInt64BatchView view;
    view.num_rows = 2;
    view.row_begin = 0;
    view.row_end = 2;
    view.value_count = 2;
    view.values = values.data();
    view.state_values = statePointers.data();
    bool initialized = true;
    auto initializedAt = [&](int32_t) { return initialized; };
    auto markAt = [&](int32_t) { initialized = false; };
    CHECK_EQ(
        omni::subop::SumRawPacked(
            view,
            initializedAt,
            markAt,
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 41);
    CHECK(initialized);

#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        CHECK_EQ(
            omni::subop::SumRawPacked(
                view,
                initializedAt,
                markAt,
                omni::subop::SUM_EXECUTION_SVE,
                nullptr),
            omni::subop::SUM_INVALID_STATE);
        CHECK_EQ(state, 41);
        CHECK(initialized);
    }
#endif
}

void TestStateBaseInvalidStateHasNoWrite() {
    int64_t state = 41;
    std::vector<int64_t> values{1, 2, 3};
    char* bases[3] = {
        reinterpret_cast<char*>(&state),
        nullptr,
        reinterpret_cast<char*>(&state)};
    omni::subop::SumInt64StateBaseView view;
    view.num_rows = 3;
    view.row_begin = 0;
    view.row_end = 3;
    view.value_count = 3;
    view.values = values.data();
    view.row_bases = bases;
    bool initialized = true;
    CHECK_EQ(
        omni::subop::SumStateBasePacked(
            view,
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = false; },
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 41);
    CHECK(initialized);
}

void TestGroupedDirect() {
    std::vector<int64_t> values{5, 7, -2, 10, 0};
    std::vector<uint8_t> nulls{0, 0, 0, 1, 0};
    std::vector<int64_t> states{10, 20};
    std::vector<bool> initialized{true, false};
    std::vector<int32_t> groups{0, 1, 0, 1, 1};
    auto stateValueAt = [&](int32_t row) {
        return &states[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto initializedAt = [&](int32_t row) {
        return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto markAt = [&](int32_t row) {
        initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
    };
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::SumRawGroupedDirect(
            5,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
            stateValueAt,
            initializedAt,
            markAt,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0], 13);
    CHECK_EQ(states[1], 7);
    CHECK_EQ(result.counted_rows, 4u);
}
} // namespace

int main() {
    TestSingleGroup();
    TestGroupedRawAndMerge();
    TestWrapAndInvalidState();
    TestPackedDenseUnroll();
    TestPackedScalarSveAndDuplicates();
    TestPackedDictionaryAndConstant();
    TestPackedInvalidStateHasNoWrite();
    TestStateBaseInvalidStateHasNoWrite();
    TestGroupedDirect();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
