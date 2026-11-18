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
} // namespace

int main() {
    TestGroupedMinMax();
    TestSingleAndInvalid();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
