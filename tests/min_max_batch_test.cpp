#include "kernel/min_max_batch.h"

#include <cstdint>
#include <cstdio>
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

void TestGroupedMinMax() {
    std::vector<int64_t> values{9, -1, 7, 0, 5};
    std::vector<uint8_t> nulls{0, 0, 1, 0, 0};
    std::vector<int32_t> groups{0, 1, 0, 1, 0};
    std::vector<int64_t> states{0, 0};
    std::vector<bool> initialized{false, false};
    std::vector<int64_t*> pointers{&states[0], &states[1], &states[0], &states[1], &states[0]};

    auto valueAt = [&](int32_t row) { return values[static_cast<size_t>(row)]; };
    auto nullAt = [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; };
    auto stateAt = [&](int32_t row) { return pointers[static_cast<size_t>(row)]; };
    auto initializedAt = [&](int32_t row) {
        return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto markAt = [&](int32_t row) {
        initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
    };

    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::MinMaxRawGroupedDirect(
            5,
            valueAt,
            nullAt,
            stateAt,
            initializedAt,
            markAt,
            omni::subop::MIN_MAX_MIN,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0], 5);
    CHECK_EQ(states[1], -1);
    CHECK(initialized[0]);
    CHECK(initialized[1]);
    CHECK_EQ(result.counted_rows, 4u);
    CHECK_EQ(result.null_skipped_rows, 1u);

    states = {0, 0};
    initialized = {false, false};
    CHECK_EQ(
        omni::subop::MinMaxRawGroupedDirect(
            5,
            valueAt,
            nullAt,
            stateAt,
            initializedAt,
            markAt,
            omni::subop::MIN_MAX_MAX,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0], 9);
    CHECK_EQ(states[1], 0);
}

void TestSingleAndInvalid() {
    std::vector<int64_t> values{4, 8, -1};
    std::vector<uint8_t> nulls{0, 1, 0};
    int64_t state = 10;
    bool initialized = true;
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::MinMaxRawSingleGroup(
            3,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
            &state,
            initialized,
            &initialized,
            omni::subop::MIN_MAX_MAX,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(state, 10);

    state = 42;
    initialized = true;
    CHECK_EQ(
        omni::subop::MinMaxRawGroupedDirect(
            2,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [&](int32_t) { return false; },
            [&](int32_t row) -> int64_t* { return row == 0 ? &state : nullptr; },
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = false; },
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 42);
    CHECK(initialized);
}

void TestStateCallbackGrouping() {
    struct Group {
        int64_t value = 0;
        bool initialized = false;
    };
    std::vector<Group> groups(2);
    const std::vector<int32_t> rowsToGroup{0, 1, 0};
    const std::vector<int64_t> values{5, 9, 3};
    auto stateAddressAt = [&](int32_t row) -> const void* {
        return &groups[static_cast<size_t>(rowsToGroup[row])];
    };
    auto loadStateAt = [&](int32_t row) {
        const auto& group = groups[static_cast<size_t>(rowsToGroup[row])];
        return omni::subop::MinMaxState{group.value, group.initialized};
    };
    auto storeStateAt = [&](int32_t row, omni::subop::MinMaxState state) {
        auto& group = groups[static_cast<size_t>(rowsToGroup[row])];
        group.value = state.value;
        group.initialized = state.initialized;
    };
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::MinMaxStateGrouped(
            3,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [](int32_t) { return false; },
            stateAddressAt,
            loadStateAt,
            storeStateAt,
            omni::subop::MIN_MAX_MIN,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(groups[0].value, 3);
    CHECK_EQ(groups[1].value, 9);
    CHECK(groups[0].initialized);
    CHECK(groups[1].initialized);
    CHECK_EQ(result.counted_rows, 3u);
    CHECK_EQ(result.groups_touched, 0u);

    groups[0] = {0, false};
    CHECK_EQ(
        omni::subop::MinMaxStateGrouped(
            2,
            [](int32_t row) { return row == 0 ? 4 : 8; },
            [](int32_t) { return false; },
            [&](int32_t) -> const void* { return &groups[0]; },
            [&](int32_t) {
                return omni::subop::MinMaxState{groups[0].value, groups[0].initialized};
            },
            [&](int32_t, omni::subop::MinMaxState state) {
                groups[0].value = state.value;
                groups[0].initialized = state.initialized;
            },
            omni::subop::MIN_MAX_MAX,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(groups[0].value, 8);
    CHECK_EQ(result.groups_touched, 1u);
}

void TestStateCallbackNoWriteOnInvalidState() {
    int64_t state = 42;
    bool initialized = true;
    const std::vector<int64_t> values{11, 7};
    CHECK_EQ(
        omni::subop::MinMaxStateGrouped(
            2,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [](int32_t) { return false; },
            [&](int32_t row) -> const void* {
                return row == 0 ? &state : nullptr;
            },
            [&](int32_t) {
                return omni::subop::MinMaxState{state, initialized};
            },
            [&](int32_t, omni::subop::MinMaxState next) {
                state = next.value;
                initialized = next.initialized;
            },
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 42);
    CHECK(initialized);
}

void TestPackedMinMax() {
    constexpr int32_t rows = 70;
    std::vector<int64_t> values(static_cast<size_t>(rows));
    std::vector<int32_t> groups(static_cast<size_t>(rows));
    std::vector<uint64_t> active((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<uint64_t> nulls((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<int64_t> stateValues{100, -100, 0};
    std::vector<bool> initialized{false, false, false};
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
        statePointers[i] = &stateValues[static_cast<size_t>(groups[i])];
    }

    auto initializedAt = [&](int32_t row) {
        return initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto markAt = [&](int32_t row) {
        initialized[static_cast<size_t>(groups[static_cast<size_t>(row)])] = true;
    };
    omni::subop::Int64BatchView view;
    view.num_rows = rows;
    view.row_begin = 0;
    view.row_end = rows;
    view.value_count = rows;
    view.values = values.data();
    view.state_values = statePointers.data();
    view.active_mask = active.data();
    view.null_mask = nulls.data();

    omni::subop::SumResult result{};
    auto denseView = view;
    denseView.active_mask = nullptr;
    denseView.null_mask = nullptr;
    stateValues = {100, -100, 0};
    initialized = {false, false, false};
    CHECK_EQ(
        omni::subop::MinMaxRawPacked(
            denseView,
            initializedAt,
            markAt,
            omni::subop::MIN_MAX_MIN,
            omni::subop::MIN_MAX_SCALAR,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(stateValues[0], -20);
    CHECK_EQ(stateValues[1], -19);
    CHECK_EQ(stateValues[2], -18);
    CHECK_EQ(result.counted_rows, 70u);

    stateValues = {100, -100, 0};
    initialized = {false, false, false};
    CHECK_EQ(
        omni::subop::MinMaxRawPacked(
            denseView,
            initializedAt,
            markAt,
            omni::subop::MIN_MAX_MAX,
            omni::subop::MIN_MAX_SCALAR,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(stateValues[0], 49);
    CHECK_EQ(stateValues[1], 47);
    CHECK_EQ(stateValues[2], 48);

    CHECK_EQ(
        omni::subop::MinMaxRawPacked(
            view,
            initializedAt,
            markAt,
            omni::subop::MIN_MAX_MIN,
            omni::subop::MIN_MAX_SCALAR,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(stateValues[0], -17);
    CHECK_EQ(stateValues[1], -19);
    CHECK_EQ(stateValues[2], -18);
    CHECK_EQ(result.counted_rows, 54u);

    stateValues = {100, -100, 0};
    initialized = {false, false, false};
#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        CHECK_EQ(
            omni::subop::MinMaxRawPacked(
                view,
                initializedAt,
                markAt,
                omni::subop::MIN_MAX_MAX,
                omni::subop::MIN_MAX_SVE,
                nullptr),
            omni::subop::SUM_OK);
        CHECK_EQ(stateValues[0], 49);
        CHECK_EQ(stateValues[1], 47);
        CHECK_EQ(stateValues[2], 48);
    }
#endif
}

void TestPackedInvalidStateNoWrite() {
    constexpr int32_t rows = 2;
    std::vector<int64_t> values{11, 7};
    std::vector<int64_t*> statePointers{nullptr, nullptr};
    int64_t state = 42;
    bool initialized = true;
    statePointers[0] = &state;
    std::vector<uint64_t> active{0x3ULL};
    omni::subop::Int64BatchView view;
    view.num_rows = rows;
    view.row_begin = 0;
    view.row_end = rows;
    view.value_count = rows;
    view.values = values.data();
    view.state_values = statePointers.data();
    view.active_mask = active.data();
    CHECK_EQ(
        omni::subop::MinMaxRawPacked(
            view,
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = false; },
            omni::subop::MIN_MAX_MIN,
            omni::subop::MIN_MAX_SCALAR,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 42);
    CHECK(initialized);
}

void TestStateBaseInvalidStateHasNoWrite() {
    int64_t state = 42;
    std::vector<int64_t> values{11, 7, 3};
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
        omni::subop::MinMaxStateBasePacked(
            view,
            omni::subop::MIN_MAX_MIN,
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = false; },
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 42);
    CHECK(initialized);
}

} // namespace

int main() {
    TestGroupedMinMax();
    TestSingleAndInvalid();
    TestStateCallbackGrouping();
    TestStateCallbackNoWriteOnInvalidState();
    TestPackedMinMax();
    TestPackedInvalidStateNoWrite();
    TestStateBaseInvalidStateHasNoWrite();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
