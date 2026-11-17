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

} // namespace

int main() {
    TestSingleGroup();
    TestGroupedRawAndMerge();
    TestWrapAndInvalidState();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
