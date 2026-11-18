#include "kernel/decimal_sum_batch.h"

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

void TestDecimalArithmetic() {
#if defined(__SIZEOF_INT128__)
    constexpr omni::subop::DecimalUInt128 kMaxBits =
        (static_cast<omni::subop::DecimalUInt128>(1) << 127) - 1;
    const omni::subop::DecimalInt128 maxValue =
        static_cast<omni::subop::DecimalInt128>(kMaxBits);
    const omni::subop::DecimalInt128 one = 1;
    omni::subop::DecimalInt128 result = 0;

    CHECK_EQ(
        omni::subop::decimal_sum_detail::AddWithOverflow(
            maxValue, one, &result),
        1);
    CHECK_EQ(result, 0);

    const omni::subop::DecimalInt128 negative = -maxValue;
    CHECK_EQ(
        omni::subop::decimal_sum_detail::AddWithOverflow(
            negative, -one, &result),
        -1);
    CHECK_EQ(result, 0);

    CHECK_EQ(
        omni::subop::decimal_sum_detail::AddWithOverflow(
            maxValue, -one, &result),
        0);
    CHECK_EQ(result, maxValue - 1);

    CHECK(omni::subop::DecimalSumAdjustForOverflow(-1, 1, &result));
    CHECK_EQ(result, maxValue);
    CHECK(omni::subop::DecimalSumAdjustForOverflow(1, -1, &result));
    CHECK_EQ(result, static_cast<omni::subop::DecimalInt128>(-maxValue));
    CHECK(!omni::subop::DecimalSumAdjustForOverflow(0, 2, &result));
#endif
}

void TestDecimalGroupedAndStrict() {
    std::vector<omni::subop::DecimalInt128> values{
        2, 3, 0, 5, 7};
    std::vector<uint8_t> nulls{0, 0, 1, 0, 0};
    std::vector<int32_t> groups{0, 1, 0, 1, 0};
    std::vector<omni::subop::DecimalSumState> states(2);
    std::vector<omni::subop::DecimalSumState*> pointers{
        &states[0], &states[1], &states[0], &states[1], &states[0]};

    omni::subop::SumResult result{};
    const auto status = omni::subop::DecimalSumRawGrouped(
        5,
        [&](int32_t row) { return values[static_cast<size_t>(row)]; },
        [&](int32_t row) { return nulls[static_cast<size_t>(row)] != 0; },
        [&](int32_t row) { return static_cast<const void*>(
            pointers[static_cast<size_t>(row)]); },
        [&](int32_t row) { return states[static_cast<size_t>(groups[static_cast<size_t>(row)])]; },
        [&](int32_t row, omni::subop::DecimalSumState state) {
            states[static_cast<size_t>(groups[static_cast<size_t>(row)])] = state;
        },
        omni::subop::DECIMAL_SUM_WRAP_TRACK,
        &result);
    CHECK_EQ(status, omni::subop::SUM_OK);
    CHECK_EQ(states[0].sum, 9);
    CHECK_EQ(states[1].sum, 8);
    CHECK_EQ(states[0].count, 2);
    CHECK_EQ(states[1].count, 2);
    CHECK_EQ(result.selected_rows, 5u);
    CHECK_EQ(result.counted_rows, 4u);
    CHECK_EQ(result.null_skipped_rows, 1u);

#if defined(__SIZEOF_INT128__)
    const omni::subop::DecimalUInt128 kMaxBits =
        (static_cast<omni::subop::DecimalUInt128>(1) << 127) - 1;
    states[0] = {};
    states[0].sum = static_cast<omni::subop::DecimalInt128>(kMaxBits);
    states[0].count = 1;
    std::vector<omni::subop::DecimalInt128> overflowValues{1};
    omni::subop::DecimalSumState* overflowPointer = &states[0];
    CHECK_EQ(
        omni::subop::DecimalSumRawGrouped(
            1,
            [&](int32_t) { return overflowValues[0]; },
            [&](int32_t) { return false; },
            [&](int32_t) { return static_cast<const void*>(overflowPointer); },
            [&](int32_t) { return states[0]; },
            [&](int32_t, omni::subop::DecimalSumState state) { states[0] = state; },
            omni::subop::DECIMAL_SUM_STRICT_NULL,
            nullptr),
        omni::subop::SUM_OK);
    CHECK(states[0].invalid);
    CHECK_EQ(states[0].count, -1);
#endif
}

void TestDecimalInvalidAndZero() {
    omni::subop::DecimalSumState state;
    omni::subop::DecimalSumState* pointer = &state;
    omni::subop::DecimalInt128 zero = 0;
    const auto status = omni::subop::DecimalSumRawGrouped(
        1,
        [&](int32_t) { return zero; },
        [&](int32_t) { return false; },
        [&](int32_t) { return static_cast<const void*>(pointer); },
        [&](int32_t) { return state; },
        [&](int32_t, omni::subop::DecimalSumState next) { state = next; },
        omni::subop::DECIMAL_SUM_WRAP_TRACK,
        nullptr);
    CHECK_EQ(status, omni::subop::SUM_OK);
    CHECK_EQ(state.sum, 0);
    CHECK_EQ(state.count, 1);

    state.sum = 5;
    state.count = 1;
    omni::subop::DecimalSumState before = state;
    CHECK_EQ(
        omni::subop::DecimalSumRawGrouped(
            2,
            [&](int32_t row) { return row == 0 ? zero : zero; },
            [&](int32_t) { return false; },
            [&](int32_t row) {
                return row == 0 ? static_cast<const void*>(&state)
                                : static_cast<const void*>(nullptr);
            },
            [&](int32_t) { return state; },
            [&](int32_t, omni::subop::DecimalSumState next) { state = next; },
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state.sum, before.sum);
    CHECK_EQ(state.count, before.count);
}
} // namespace

int main() {
    TestDecimalArithmetic();
    TestDecimalGroupedAndStrict();
    TestDecimalInvalidAndZero();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}