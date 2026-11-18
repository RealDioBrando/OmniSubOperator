#include "kernel/min_max_batch.h"

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
    std::vector<int64_t> values;
    std::vector<uint8_t> nulls;
    std::vector<int64_t> states;
    std::vector<bool> initialized;
    std::vector<int64_t*> statePtrs;
};

Data MakeData(int32_t rows, int32_t groups) {
    Data d;
    d.rows = rows;
    d.groups = groups;
    d.groupIds.resize(static_cast<size_t>(rows));
    d.values.resize(static_cast<size_t>(rows));
    d.nulls.resize(static_cast<size_t>(rows), 0);
    d.states.resize(static_cast<size_t>(groups), 0);
    d.initialized.assign(static_cast<size_t>(groups), false);
    d.statePtrs.resize(static_cast<size_t>(rows), nullptr);
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        d.groupIds[i] = row % groups;
        d.values[i] = static_cast<int64_t>(row % 1000) - 500;
        if (row % 10 == 0) {
            d.nulls[i] = 1;
        }
        d.statePtrs[i] = &d.states[static_cast<size_t>(d.groupIds[i])];
    }
    return d;
}

template <typename Fn>
double Measure(Fn&& fn, const Data& data, int32_t repeats) {
    volatile int64_t sink = 0;
    std::vector<int64_t> states;
    std::vector<uint8_t> initialized;
    for (int32_t warmup = 0; warmup < 2; ++warmup) {
        states.assign(static_cast<size_t>(data.groups), 0);
        initialized.assign(static_cast<size_t>(data.groups), 0);
        fn(states, initialized);
        sink += states[0];
    }
    std::vector<double> samples;
    for (int32_t repeat = 0; repeat < repeats; ++repeat) {
        states.assign(static_cast<size_t>(data.groups), 0);
        initialized.assign(static_cast<size_t>(data.groups), 0);
        const auto begin = Clock::now();
        fn(states, initialized);
        const auto end = Clock::now();
        sink += states[0];
        samples.push_back(
            std::chrono::duration<double, std::nano>(end - begin).count() /
            data.rows);
    }
    std::sort(samples.begin(), samples.end());
    (void)sink;
    return samples[samples.size() / 2];
}

void Run(const std::string& name, const Data& input, int32_t repeats) {
    const auto native = Measure([&](std::vector<int64_t>& states, std::vector<uint8_t>& initialized) {
        for (int32_t row = 0; row < input.rows; ++row) {
            if (input.nulls[static_cast<size_t>(row)] != 0) {
                continue;
            }
            const auto group = static_cast<size_t>(input.groupIds[static_cast<size_t>(row)]);
            const auto value = input.values[static_cast<size_t>(row)];
            if (!initialized[group]) {
                states[group] = value;
                initialized[group] = 1;
            } else if (value < states[group]) {
                states[group] = value;
            }
        }
    }, input, repeats);

    const auto shared = Measure([&](std::vector<int64_t>& states, std::vector<uint8_t>& initialized) {
        const auto status = omni::subop::MinMaxRawGroupedDirect(
            input.rows,
            [&](int32_t row) { return input.values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return input.nulls[static_cast<size_t>(row)] != 0; },
            [&](int32_t row) { return &states[static_cast<size_t>(input.groupIds[static_cast<size_t>(row)])]; },
            [&](int32_t row) {
                return initialized[static_cast<size_t>(input.groupIds[static_cast<size_t>(row)])] != 0;
            },
            [&](int32_t row) {
                initialized[static_cast<size_t>(input.groupIds[static_cast<size_t>(row)])] = 1;
            },
            omni::subop::MIN_MAX_MIN,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, input, repeats);

    std::printf("case=%s,native=%.3f,shared_callback=%.3f\n",
        name.c_str(), native, shared);
}
} // namespace

int main() {
    constexpr int32_t rows = 1000000;
    constexpr int32_t repeats = 7;
    std::printf("min/max microbenchmark, rows=%d, repeats=%d\n", rows, repeats);
    Run("min_g64_null10", MakeData(rows, 64), repeats);
    return 0;
}
