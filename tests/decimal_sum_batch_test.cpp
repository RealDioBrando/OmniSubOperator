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

    CHECK_EQ(
        omni::subop::decimal_sum_detail::AddWithOverflow(
            static_cast<omni::subop::DecimalInt128>(-300),
            static_cast<omni::subop::DecimalInt128>(-1250),
            &result),
        0);
    CHECK_EQ(result, static_cast<omni::subop::DecimalInt128>(-1550));
    CHECK_EQ(
        omni::subop::decimal_sum_detail::AddWithOverflow(
            result,
            static_cast<omni::subop::DecimalInt128>(6400),
            &result),
        0);
    CHECK_EQ(result, static_cast<omni::subop::DecimalInt128>(4850));

    omni::subop::DecimalSumState mixedSignState;
    CHECK(omni::subop::decimal_sum_detail::ApplyInput(
        mixedSignState,
        static_cast<omni::subop::DecimalInt128>(-300),
        omni::subop::DECIMAL_SUM_WRAP_TRACK));
    CHECK(omni::subop::decimal_sum_detail::ApplyInput(
        mixedSignState,
        static_cast<omni::subop::DecimalInt128>(-1250),
        omni::subop::DECIMAL_SUM_WRAP_TRACK));
    CHECK(omni::subop::decimal_sum_detail::ApplyInput(
        mixedSignState,
        static_cast<omni::subop::DecimalInt128>(6400),
        omni::subop::DECIMAL_SUM_WRAP_TRACK));
    CHECK_EQ(mixedSignState.sum, static_cast<omni::subop::DecimalInt128>(4850));
    CHECK_EQ(mixedSignState.overflow, 0);
    CHECK_EQ(mixedSignState.count, 3);

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

void TestPackedDecimal() {
#if defined(__SIZEOF_INT128__)
    constexpr int32_t rows = 70;
    std::vector<omni::subop::DecimalInt128> values(static_cast<size_t>(rows));
    std::vector<int32_t> groups(static_cast<size_t>(rows));
    std::vector<omni::subop::DecimalSumState> actual(3);
    std::vector<omni::subop::DecimalSumState> expected(3);
    std::vector<uint64_t> active((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<uint64_t> nulls((static_cast<size_t>(rows) + 63) / 64, 0);
    std::vector<const void*> stateAddresses(static_cast<size_t>(rows), nullptr);
    uint64_t expectedCounted = 0;
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        values[i] = static_cast<omni::subop::DecimalInt128>((row % 7) - 3);
        groups[i] = row % 3;
        if (row % 5 != 0) {
            active[i / 64] |= 1ULL << (i % 64);
        }
        if (row % 11 == 0) {
            nulls[i / 64] |= 1ULL << (i % 64);
        }
    }
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        if ((active[i / 64] & (1ULL << (i % 64))) == 0 ||
            (nulls[i / 64] & (1ULL << (i % 64))) != 0) {
            continue;
        }
        ++expectedCounted;
        const auto group = static_cast<size_t>(groups[i]);
        stateAddresses[i] = &actual[group];
        omni::subop::decimal_sum_detail::ApplyInput(
            expected[group],
            values[i],
            omni::subop::DECIMAL_SUM_WRAP_TRACK);
    }

    auto loadAt = [&](int32_t row) {
        return actual[static_cast<size_t>(groups[static_cast<size_t>(row)])];
    };
    auto storeAt = [&](int32_t row, omni::subop::DecimalSumState state) {
        actual[static_cast<size_t>(groups[static_cast<size_t>(row)])] = state;
    };
    omni::subop::DecimalSumBatchView view;
    view.num_rows = rows;
    view.row_begin = 0;
    view.row_end = rows;
    view.value_count = rows;
    view.values = values.data();
    view.state_values = stateAddresses.data();
    view.active_mask = active.data();
    view.null_mask = nulls.data();
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::DecimalSumRawPacked(
            view,
            loadAt,
            storeAt,
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            omni::subop::SUM_EXECUTION_SCALAR,
            &result),
        omni::subop::SUM_OK);
    for (size_t group = 0; group < actual.size(); ++group) {
        CHECK_EQ(actual[group].sum, expected[group].sum);
        CHECK_EQ(actual[group].count, expected[group].count);
        CHECK_EQ(actual[group].overflow, expected[group].overflow);
    }
    CHECK_EQ(result.counted_rows, expectedCounted);

    std::vector<omni::subop::DecimalSumState> expectedDense(3);
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        omni::subop::decimal_sum_detail::ApplyInput(
            expectedDense[static_cast<size_t>(groups[i])],
            values[i],
            omni::subop::DECIMAL_SUM_WRAP_TRACK);
    }
    actual.assign(3, {});
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        stateAddresses[i] = &actual[static_cast<size_t>(groups[i])];
    }
    auto denseView = view;
    denseView.active_mask = nullptr;
    denseView.null_mask = nullptr;
    CHECK_EQ(
        omni::subop::DecimalSumRawPacked(
            denseView,
            loadAt,
            storeAt,
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_OK);
    for (size_t group = 0; group < actual.size(); ++group) {
        CHECK_EQ(actual[group].sum, expectedDense[group].sum);
        CHECK_EQ(actual[group].count, expectedDense[group].count);
    }

#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        actual.assign(actual.size(), {});
        for (int32_t row = 0; row < rows; ++row) {
            const auto i = static_cast<size_t>(row);
            if ((active[i / 64] & (1ULL << (i % 64))) != 0 &&
                (nulls[i / 64] & (1ULL << (i % 64))) == 0) {
                stateAddresses[i] = &actual[static_cast<size_t>(groups[i])];
            }
        }
        CHECK_EQ(
            omni::subop::DecimalSumRawPacked(
                view,
                loadAt,
                storeAt,
                omni::subop::DECIMAL_SUM_WRAP_TRACK,
                omni::subop::SUM_EXECUTION_SVE,
                &result),
            omni::subop::SUM_OK);
        for (size_t group = 0; group < actual.size(); ++group) {
            CHECK_EQ(actual[group].sum, expected[group].sum);
            CHECK_EQ(actual[group].count, expected[group].count);
        }
    }
#endif
#endif
}

void TestPackedDecimalInvalidStateNoWrite() {
    std::vector<omni::subop::DecimalInt128> values{5, 7};
    omni::subop::DecimalSumState state;
    state.sum = 11;
    state.count = 2;
    std::vector<const void*> addresses{&state, nullptr};
    std::vector<uint64_t> active{0x3ULL};
    omni::subop::DecimalSumBatchView view;
    view.num_rows = 2;
    view.row_begin = 0;
    view.row_end = 2;
    view.value_count = 2;
    view.values = values.data();
    view.state_values = addresses.data();
    view.active_mask = active.data();
    CHECK_EQ(
        omni::subop::DecimalSumRawPacked(
            view,
            [&](int32_t) { return state; },
            [&](int32_t, omni::subop::DecimalSumState next) { state = next; },
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state.sum, 11);
    CHECK_EQ(state.count, 2);
}

void TestDecimalPartialMerge() {
    std::vector<omni::subop::DecimalSumPartial> partials{
        {5, 2}, {7, 3}, {0, 0}, {0, -1}};
    std::vector<int32_t> groups{0, 1, 0, 0};
    std::vector<omni::subop::DecimalSumState> states(2);
    std::vector<omni::subop::DecimalSumState*> pointers{
        &states[0], &states[1], &states[0], &states[0]};
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::DecimalSumMergeGrouped(
            4,
            [&](int32_t row) { return partials[static_cast<size_t>(row)]; },
            [&](int32_t) { return false; },
            [&](int32_t row) -> const void* {
                return pointers[static_cast<size_t>(row)];
            },
            [&](int32_t row) { return states[static_cast<size_t>(groups[static_cast<size_t>(row)])]; },
            [&](int32_t row, omni::subop::DecimalSumState state) {
                states[static_cast<size_t>(groups[static_cast<size_t>(row)])] = state;
            },
            omni::subop::DECIMAL_SUM_STRICT_NULL,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0].count, -1);
    CHECK(states[0].invalid);
    CHECK_EQ(states[1].sum, 7);
    CHECK_EQ(states[1].count, 3);
    CHECK_EQ(result.counted_rows, 4u);
}

void TestDecimalPartialInvalidStateNoWrite() {
    std::vector<omni::subop::DecimalSumPartial> partials{{4, 1}, {9, 1}};
    omni::subop::DecimalSumState state;
    state.sum = 11;
    state.count = 2;
    omni::subop::DecimalSumState* pointers[2] = {&state, nullptr};
    CHECK_EQ(
        omni::subop::DecimalSumMergeGrouped(
            2,
            [&](int32_t row) { return partials[static_cast<size_t>(row)]; },
            [&](int32_t) { return false; },
            [&](int32_t row) -> const void* { return pointers[row]; },
            [&](int32_t) { return state; },
            [&](int32_t, omni::subop::DecimalSumState next) { state = next; },
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state.sum, 11);
    CHECK_EQ(state.count, 2);
}

void TestDecimalStateBaseInvalidStateNoWrite() {
#if defined(__SIZEOF_INT128__)
    std::vector<int64_t> values{5, 7, 9};
    omni::subop::DecimalSumState state;
    state.sum = 11;
    state.count = 2;
    char* bases[3] = {
        reinterpret_cast<char*>(&state),
        nullptr,
        reinterpret_cast<char*>(&state)};
    auto loadAt = [&](int32_t row) {
        return *reinterpret_cast<omni::subop::DecimalSumState*>(bases[row]);
    };
    auto storeAt = [&](int32_t row, omni::subop::DecimalSumState next) {
        *reinterpret_cast<omni::subop::DecimalSumState*>(bases[row]) = next;
    };
    CHECK_EQ(
        omni::subop::DecimalSumStateBaseFlat<int64_t>(
            3,
            0,
            3,
            values.data(),
            bases,
            0,
            nullptr,
            nullptr,
            loadAt,
            storeAt,
            [](int32_t) {},
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state.sum, 11);
    CHECK_EQ(state.count, 2);
#endif
}

} // namespace

int main() {
    TestDecimalArithmetic();
    TestDecimalGroupedAndStrict();
    TestDecimalInvalidAndZero();
    TestPackedDecimal();
    TestPackedDecimalInvalidStateNoWrite();
    TestDecimalPartialMerge();
    TestDecimalPartialInvalidStateNoWrite();
    TestDecimalStateBaseInvalidStateNoWrite();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}
