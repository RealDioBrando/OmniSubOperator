#include "adapters/omniruntime/CountBatch.h"
#include "adapters/omniruntime/DecimalSumBatch.h"
#include "adapters/omniruntime/MinMaxBatch.h"
#include "adapters/omniruntime/SumBatch.h"

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

uint8_t* Bytes(void* value) {
    return reinterpret_cast<uint8_t*>(value);
}

constexpr bool NoNull(size_t) {
    return false;
}

void TestCountFilteredMask() {
    int64_t groupedState = 10;
    std::vector<uint8_t*> states{
        Bytes(&groupedState), Bytes(&groupedState), Bytes(&groupedState)};
    const std::vector<uint8_t> nulls{0, 1, 0};
    OmniSubOpCountResult result{};
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            states,
            false,
            [&](size_t row) { return nulls[row] != 0; },
            0,
            &result),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(groupedState, 12);
    CHECK_EQ(result.selected_rows, 2u);
    CHECK_EQ(result.counted_rows, 2u);
    CHECK_EQ(result.null_skipped_rows, 0u);
    CHECK_EQ(result.groups_touched, 1u);

    int64_t singleState = 4;
    CHECK_EQ(
        omni::subop::omniruntime::CountRawSingleGroup(
            3,
            &singleState,
            false,
            [&](size_t row) { return nulls[row] != 0; },
            nullptr),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(singleState, 6);
}

void TestCountDenseRouting() {
    int64_t state = 0;
    constexpr int32_t routedRows =
        omni::subop::omniruntime::kDenseCountNativeBatchLimit;
    std::vector<uint8_t*> routedStates(
        static_cast<size_t>(routedRows), Bytes(&state));
    OmniSubOpCountResult untouched{};
    untouched.selected_rows = 99;
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            routedStates, true, NoNull, 0, &untouched),
        OMNI_SUBOP_COUNT_UNSUPPORTED_ENCODING);
    CHECK_EQ(state, 0);
    CHECK_EQ(untouched.selected_rows, 99u);

    int64_t smallState = 0;
    std::vector<uint8_t*> smallStates{Bytes(&smallState), Bytes(&smallState)};
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            smallStates, true, NoNull, 0, nullptr),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(smallState, 2);

    std::vector<uint8_t*> sharedStates(
        static_cast<size_t>(routedRows) + 1, Bytes(&state));
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            sharedStates, true, NoNull, 0, nullptr),
        OMNI_SUBOP_COUNT_OK);
    CHECK_EQ(state, static_cast<int64_t>(routedRows) + 1);
}

void TestCountNoWriteOnInvalidState() {
    int64_t state = 17;
    std::vector<uint8_t*> states{Bytes(&state), nullptr};
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            states, false, NoNull, 0, nullptr),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(state, 17);

    OmniSubOpCountResult result{};
    CHECK_EQ(
        omni::subop::omniruntime::CountRawBatch(
            states, false, NoNull, 0, &result),
        OMNI_SUBOP_COUNT_INVALID_STATE);
    CHECK_EQ(state, 17);
}

void TestSumCallbackSemantics() {
    int64_t state = 10;
    std::vector<uint8_t*> states{Bytes(&state), Bytes(&state), Bytes(&state)};
    bool initialized = true;
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::omniruntime::SumRawGrouped(
            3,
            [](int32_t row) { return row + 1; },
            NoNull,
            [&](int32_t) { return &state; },
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(state, 16);

    int64_t emptyState = 0;
    initialized = false;
    CHECK_EQ(
        omni::subop::omniruntime::SumRawGrouped(
            2,
            [](int32_t row) { return row + 1; },
            NoNull,
            [&](int32_t) { return &emptyState; },
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(emptyState, 3);

    int64_t singleState = 5;
    CHECK_EQ(
        omni::subop::omniruntime::SumRawSingleGroup(
            3,
            [](int32_t row) { return row + 1; },
            [](int32_t row) { return row == 1; },
            &singleState,
            true,
            nullptr,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(singleState, 9);
    CHECK_EQ(result.counted_rows, 2u);

    int64_t mergedState = 0;
    initialized = false;
    CHECK_EQ(
        omni::subop::omniruntime::SumMergeGrouped(
            3,
            [](int32_t row) { return row + 10; },
            NoNull,
            [&](int32_t) { return &mergedState; },
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(mergedState, 33);
}

void TestSumNoWriteOnInvalidState() {
    int64_t state = 23;
    std::vector<uint8_t*> states{Bytes(&state), nullptr};
    auto stateAt = [&](int32_t row) -> int64_t* {
        return row == 0 ? &state : nullptr;
    };
    auto initialized = [](int32_t) { return true; };
    auto mark = [](int32_t) {};
    CHECK_EQ(
        omni::subop::omniruntime::SumRawGrouped(
            2,
            [](int32_t row) { return row + 1; },
            NoNull,
            stateAt,
            initialized,
            mark,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state, 23);
}

void TestSumNullSkipsInvalidAddress() {
    int64_t state = 23;
    std::vector<uint8_t*> states{Bytes(&state), nullptr};
    auto stateAt = [&](int32_t row) -> int64_t* {
        return row == 0 ? &state : nullptr;
    };
    bool initialized = true;
    CHECK_EQ(
        omni::subop::omniruntime::SumRawGrouped(
            2,
            [](int32_t row) { return row + 1; },
            [](int32_t row) { return row == 1; },
            stateAt,
            [&](int32_t) { return initialized; },
            [&](int32_t) { initialized = true; },
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(state, 24);
}

struct MinMaxGroup {
    int64_t value = 0;
    bool initialized = false;
};

void TestMinMaxCallbackSemantics() {
    std::vector<MinMaxGroup> groups(2);
    std::vector<int32_t> rowsToGroup{0, 1, 0};
    std::vector<uint8_t*> states{
        Bytes(&groups[0]), Bytes(&groups[1]), Bytes(&groups[0])};
    const std::vector<int64_t> values{5, 9, 3};
    auto load = [&](int32_t row) {
        const auto& group = groups[static_cast<size_t>(rowsToGroup[row])];
        return omni::subop::MinMaxState{group.value, group.initialized};
    };
    auto store = [&](int32_t row, omni::subop::MinMaxState state) {
        auto& group = groups[static_cast<size_t>(rowsToGroup[row])];
        group.value = state.value;
        group.initialized = state.initialized;
    };
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            states,
            0,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            NoNull,
            load,
            store,
            omni::subop::MIN_MAX_MIN,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(groups[0].value, 3);
    CHECK_EQ(groups[1].value, 9);
    CHECK(groups[0].initialized);
    CHECK(groups[1].initialized);
    CHECK_EQ(result.selected_rows, 3u);
    CHECK_EQ(result.counted_rows, 3u);
    CHECK_EQ(result.groups_touched, 0u);

    groups[0].value = -1;
    groups[0].initialized = true;
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            std::vector<uint8_t*>{Bytes(&groups[0]), Bytes(&groups[0])},
            0,
            [](int32_t row) { return row == 0 ? 4 : 8; },
            NoNull,
            [](int32_t) {
                return omni::subop::MinMaxState{-1, true};
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

void TestMinMaxNoWriteOnInvalidState() {
    MinMaxGroup group{19, true};
    std::vector<uint8_t*> states{Bytes(&group), nullptr};
    auto load = [&](int32_t) {
        return omni::subop::MinMaxState{group.value, group.initialized};
    };
    auto store = [&](int32_t, omni::subop::MinMaxState state) {
        group.value = state.value;
        group.initialized = state.initialized;
    };
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            states,
            0,
            [](int32_t row) { return row + 1; },
            NoNull,
            load,
            store,
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(group.value, 19);
    CHECK(group.initialized);
}

void TestMinMaxNullAndArgumentChecks() {
    MinMaxGroup group{0, false};
    auto load = [&](int32_t) {
        return omni::subop::MinMaxState{group.value, group.initialized};
    };
    auto store = [&](int32_t, omni::subop::MinMaxState state) {
        group.value = state.value;
        group.initialized = state.initialized;
    };
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            std::vector<uint8_t*>{Bytes(&group), nullptr},
            0,
            [](int32_t row) { return row + 1; },
            [](int32_t row) { return row != 0; },
            load,
            store,
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(group.value, 1);
    CHECK(group.initialized);

    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            std::vector<uint8_t*>{Bytes(&group)},
            -1,
            [](int32_t) { return 1; },
            NoNull,
            load,
            store,
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_INVALID_ARGUMENT);
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            std::vector<uint8_t*>{Bytes(&group)},
            0,
            [](int32_t) { return 1; },
            NoNull,
            load,
            store,
            static_cast<omni::subop::MinMaxKind>(99),
            nullptr),
        omni::subop::SUM_INVALID_ARGUMENT);
}

void TestDecimalRawSemantics() {
    std::vector<omni::subop::DecimalSumState> states(2);
    std::vector<int32_t> rowsToGroup{0, 0, 1};
    std::vector<uint8_t*> rowStates{
        Bytes(&states[0]), Bytes(&states[0]), Bytes(&states[1])};
    const std::vector<omni::subop::DecimalInt128> values{5, 7, 3};
    auto load = [&](int32_t row) {
        return states[static_cast<size_t>(rowsToGroup[row])];
    };
    auto store = [&](int32_t row, omni::subop::DecimalSumState state) {
        states[static_cast<size_t>(rowsToGroup[row])] = state;
    };
    omni::subop::SumResult result{};
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumRawGrouped(
            rowStates,
            0,
            [&](int32_t row) { return values[static_cast<size_t>(row)]; },
            [](int32_t row) { return row == 1; },
            load,
            store,
            omni::subop::DECIMAL_SUM_STRICT_NULL,
            &result),
        omni::subop::SUM_OK);
    CHECK_EQ(states[0].sum, 5);
    CHECK_EQ(states[0].count, 1);
    CHECK_EQ(states[1].sum, 3);
    CHECK_EQ(states[1].count, 1);
    CHECK_EQ(result.counted_rows, 2u);
    CHECK_EQ(result.null_skipped_rows, 1u);
}

void TestDecimalNoWriteOnInvalidState() {
    std::vector<int64_t> values{5, 7};
    std::vector<uint8_t*> states{Bytes(nullptr), nullptr};
    omni::subop::DecimalSumState state;
    state.sum = 11;
    state.count = 1;
    states[0] = Bytes(&state);
    auto load = [&](int32_t) { return state; };
    auto store = [&](int32_t, omni::subop::DecimalSumState next) {
        state = next;
    };
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumRawGrouped(
            states,
            0,
            [&](int32_t row) {
                return static_cast<omni::subop::DecimalInt128>(values[row]);
            },
            NoNull,
            load,
            store,
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_INVALID_STATE);
    CHECK_EQ(state.sum, 11);
    CHECK_EQ(state.count, 1);
}

void TestDecimalNullSkipsInvalidAddress() {
    omni::subop::DecimalSumState state;
    state.sum = 11;
    state.count = 1;
    std::vector<uint8_t*> states{Bytes(&state), nullptr};
    auto load = [&](int32_t) { return state; };
    auto store = [&](int32_t, omni::subop::DecimalSumState next) {
        state = next;
    };
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumRawGrouped(
            states,
            0,
            [](int32_t row) {
                return static_cast<omni::subop::DecimalInt128>(row + 5);
            },
            [](int32_t row) { return row == 1; },
            load,
            store,
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(state.sum, 16);
    CHECK_EQ(state.count, 2);
}

void TestDecimalPartialSemantics() {
    omni::subop::DecimalSumState state;
    std::vector<uint8_t*> states{Bytes(&state), Bytes(&state), Bytes(&state)};
    auto load = [&](int32_t) { return state; };
    auto store = [&](int32_t, omni::subop::DecimalSumState next) {
        state = next;
    };
    const std::vector<omni::subop::DecimalSumPartial> partials{
        {5, 1}, {7, 1}, {3, 1}};
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumPartialGrouped(
            states,
            0,
            [&](int32_t row) { return partials[static_cast<size_t>(row)]; },
            [](int32_t row) { return row == 2; },
            load,
            store,
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_OK);
    CHECK_EQ(state.sum, 12);
    CHECK_EQ(state.count, 2);
}

#if defined(__SIZEOF_INT128__)
void TestDecimalStrictOverflow() {
    omni::subop::DecimalSumState state;
    const auto max = static_cast<omni::subop::DecimalInt128>(
        (static_cast<omni::subop::DecimalUInt128>(1) << 127) - 1);
    state.sum = max;
    state.count = 1;
    std::vector<uint8_t*> states{Bytes(&state)};
    auto load = [&](int32_t) { return state; };
    auto store = [&](int32_t, omni::subop::DecimalSumState next) {
        state = next;
    };
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumRawGrouped(
            states,
            0,
            [](int32_t) {
                return static_cast<omni::subop::DecimalInt128>(1);
            },
            NoNull,
            load,
            store,
            omni::subop::DECIMAL_SUM_STRICT_NULL,
            nullptr),
        omni::subop::SUM_OK);
    CHECK(state.invalid);
    CHECK_EQ(state.count, -1);
}
#endif

void TestNegativeStateOffset() {
    MinMaxGroup group{1, true};
    auto load = [&](int32_t) {
        return omni::subop::MinMaxState{group.value, group.initialized};
    };
    auto store = [&](int32_t, omni::subop::MinMaxState state) {
        group.value = state.value;
    };
    CHECK_EQ(
        omni::subop::omniruntime::MinMaxRawGrouped(
            std::vector<uint8_t*>{Bytes(&group)},
            -1,
            [](int32_t) { return 1; },
            NoNull,
            load,
            store,
            omni::subop::MIN_MAX_MIN,
            nullptr),
        omni::subop::SUM_INVALID_ARGUMENT);

    omni::subop::DecimalSumState state;
    CHECK_EQ(
        omni::subop::omniruntime::DecimalSumRawGrouped(
            std::vector<uint8_t*>{Bytes(&state)},
            -1,
            [](int32_t) { return static_cast<omni::subop::DecimalInt128>(1); },
            NoNull,
            [&](int32_t) { return state; },
            [&](int32_t, omni::subop::DecimalSumState next) { state = next; },
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr),
        omni::subop::SUM_INVALID_ARGUMENT);
}

} // namespace

int main() {
    TestCountFilteredMask();
    TestCountDenseRouting();
    TestCountNoWriteOnInvalidState();
    TestSumCallbackSemantics();
    TestSumNoWriteOnInvalidState();
    TestSumNullSkipsInvalidAddress();
    TestMinMaxCallbackSemantics();
    TestMinMaxNoWriteOnInvalidState();
    TestMinMaxNullAndArgumentChecks();
    TestDecimalRawSemantics();
    TestDecimalNoWriteOnInvalidState();
    TestDecimalNullSkipsInvalidAddress();
    TestDecimalPartialSemantics();
#if defined(__SIZEOF_INT128__)
    TestDecimalStrictOverflow();
#endif
    TestNegativeStateOffset();
    if (gFailures != 0) {
        std::printf("%d/%d checks failed\n", gFailures, gChecks);
        return 1;
    }
    std::printf("all %d checks passed\n", gChecks);
    return 0;
}