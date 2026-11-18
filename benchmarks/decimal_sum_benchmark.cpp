#include "kernel/decimal_sum_batch.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Data {
    int32_t rows = 0;
    int32_t groups = 0;
    std::vector<int32_t> groupIds;
    std::vector<omni::subop::DecimalInt128> values;
    std::vector<uint8_t> nulls;
    std::vector<omni::subop::DecimalSumState> states;
    std::vector<omni::subop::DecimalSumState*> statePtrs;
};

Data MakeData(int32_t rows, int32_t groups, int32_t nullStride) {
    Data data;
    data.rows = rows;
    data.groups = groups;
    data.groupIds.resize(static_cast<size_t>(rows));
    data.values.resize(static_cast<size_t>(rows));
    data.nulls.resize(static_cast<size_t>(rows), 0);
    data.states.resize(static_cast<size_t>(groups));
    data.statePtrs.resize(static_cast<size_t>(rows), nullptr);
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        data.groupIds[i] = row % groups;
        data.values[i] = static_cast<omni::subop::DecimalInt128>((row % 1000) - 500);
        if (nullStride > 0 && row % nullStride == 0) {
            data.nulls[i] = 1;
        }
    }
    if (nullStride <= 0) {
        data.nulls.clear();
    }
    return data;
}

void FillPointers(Data& data) {
    for (int32_t row = 0; row < data.rows; ++row) {
        data.statePtrs[static_cast<size_t>(row)] =
            &data.states[static_cast<size_t>(data.groupIds[static_cast<size_t>(row)])];
    }
}

template <typename Fn>
double Measure(Fn&& fn, const Data& data, int32_t repeats) {
    volatile int64_t sink = 0;
    std::vector<omni::subop::DecimalSumState> states;
    for (int32_t warmup = 0; warmup < 2; ++warmup) {
        states.assign(static_cast<size_t>(data.groups), {});
        fn(states);
        sink += static_cast<int64_t>(states[0].count);
    }
    std::vector<double> samples;
    for (int32_t repeat = 0; repeat < repeats; ++repeat) {
        states.assign(static_cast<size_t>(data.groups), {});
        const auto begin = Clock::now();
        fn(states);
        const auto end = Clock::now();
        sink += static_cast<int64_t>(states[0].count);
        samples.push_back(
            std::chrono::duration<double, std::nano>(end - begin).count() /
            data.rows);
    }
    std::sort(samples.begin(), samples.end());
    (void)sink;
    return samples[samples.size() / 2];
}

void Run(const std::string& name, const Data& input, int32_t repeats) {
    Data data = input;
    FillPointers(data);

    const auto native = Measure([&](std::vector<omni::subop::DecimalSumState>& states) {
        for (int32_t row = 0; row < data.rows; ++row) {
            if (!data.nulls.empty() && data.nulls[static_cast<size_t>(row)] != 0) {
                continue;
            }
            auto& state = states[static_cast<size_t>(data.groupIds[static_cast<size_t>(row)])];
            omni::subop::decimal_sum_detail::ApplyInput(
                state,
                data.values[static_cast<size_t>(row)],
                omni::subop::DECIMAL_SUM_WRAP_TRACK);
        }
    }, data, repeats);

    const auto shared = Measure([&](std::vector<omni::subop::DecimalSumState>& states) {
        const auto status = omni::subop::DecimalSumRawGrouped(
            data.rows,
            [&](int32_t row) { return data.values[static_cast<size_t>(row)]; },
            [&](int32_t row) {
                return !data.nulls.empty() && data.nulls[static_cast<size_t>(row)] != 0;
            },
            [&](int32_t row) { return static_cast<const void*>(
                data.statePtrs[static_cast<size_t>(row)]); },
            [&](int32_t row) {
                return states[static_cast<size_t>(data.groupIds[static_cast<size_t>(row)])];
            },
            [&](int32_t row, omni::subop::DecimalSumState state) {
                states[static_cast<size_t>(data.groupIds[static_cast<size_t>(row)])] = state;
            },
            omni::subop::DECIMAL_SUM_WRAP_TRACK,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, data, repeats);

    std::printf("case=%s,native=%.3f,shared_callback=%.3f\n",
        name.c_str(), native, shared);
}
} // namespace

int main() {
    constexpr int32_t rows = 1000000;
    constexpr int32_t repeats = 7;
    std::printf("decimal sum microbenchmark, rows=%d, repeats=%d\n", rows, repeats);
    Run("decimal_g64_null10", MakeData(rows, 64, 10), repeats);
    Run("decimal_g64_dense", MakeData(rows, 64, 0), repeats);
    return 0;
}
