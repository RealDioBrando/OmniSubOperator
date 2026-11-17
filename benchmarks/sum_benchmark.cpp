#include "kernel/sum_batch.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Data {
    int32_t rows;
    int32_t groups;
    int32_t selected;
    std::vector<int32_t> groupIds;
    std::vector<int64_t> values;
    std::vector<uint8_t> active;
    std::vector<uint8_t> nulls;
    std::vector<int64_t*> statePtrs;
    std::vector<uint8_t*> bases;
};

Data MakeData(int32_t rows, int32_t groups, int32_t stride, int32_t nullStride) {
    Data d;
    d.rows = rows;
    d.groups = groups;
    d.groupIds.resize(static_cast<size_t>(rows));
    d.values.resize(static_cast<size_t>(rows));
    d.active.resize(static_cast<size_t>(rows), 1);
    d.nulls.resize(static_cast<size_t>(rows), 0);
    d.statePtrs.resize(static_cast<size_t>(rows), nullptr);
    d.bases.resize(static_cast<size_t>(rows), nullptr);
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        d.groupIds[i] = row % groups;
        d.values[i] = (row % 17) - 8;
        if (stride > 1) {
            d.active[i] = row % stride == 0 ? 1 : 0;
        }
        if (nullStride > 0) {
            d.nulls[i] = row % nullStride == 0 ? 1 : 0;
        }
        if (stride <= 1 || d.active[i] != 0) {
            ++d.selected;
        }
    }
    if (stride <= 1) {
        d.active.clear();
    }
    if (nullStride <= 0) {
        d.nulls.clear();
    }
    return d;
}

void FillStatePointers(Data& d, std::vector<int64_t>& states) {
    for (int32_t row = 0; row < d.rows; ++row) {
        const auto i = static_cast<size_t>(row);
        d.statePtrs[i] = &states[static_cast<size_t>(d.groupIds[i])];
        d.bases[i] = reinterpret_cast<uint8_t*>(d.statePtrs[i]);
    }
}

template <typename Fn>
double Measure(Fn&& fn, int32_t rows, std::vector<int64_t>& states, std::vector<uint8_t>& init, int32_t repeats) {
    volatile int64_t sink = 0;
    for (int32_t warmup = 0; warmup < 2; ++warmup) {
        std::fill(states.begin(), states.end(), 0);
        std::fill(init.begin(), init.end(), 0);
        fn();
        sink += states[0];
    }
    std::vector<double> samples;
    for (int32_t repeat = 0; repeat < repeats; ++repeat) {
        std::fill(states.begin(), states.end(), 0);
        std::fill(init.begin(), init.end(), 0);
        const auto begin = Clock::now();
        fn();
        const auto end = Clock::now();
        sink += states[0];
        samples.push_back(std::chrono::duration<double, std::nano>(end - begin).count() / rows);
    }
    std::sort(samples.begin(), samples.end());
    (void)sink;
    return samples[samples.size() / 2];
}

void RunRaw(const std::string& name, Data d, int32_t repeats) {
    std::vector<int64_t> states(static_cast<size_t>(d.groups), 0);
    FillStatePointers(d, states);
    std::vector<uint8_t> initialized(static_cast<size_t>(d.groups), 0);
    const auto native = Measure([&] {
        for (int32_t row = 0; row < d.rows; ++row) {
            const auto i = static_cast<size_t>(row);
            if (d.active.empty() || d.active[i] != 0) {
                if (!d.nulls.empty() && d.nulls[i] != 0) {
                    continue;
                }
                auto& state = *d.statePtrs[i];
                const auto add = static_cast<int64_t>(
                    static_cast<uint64_t>(state) + static_cast<uint64_t>(d.values[i]));
                state = initialized[static_cast<size_t>(d.groupIds[i])] ? add : d.values[i];
                initialized[static_cast<size_t>(d.groupIds[i])] = 1;
            }
        }
    }, d.rows, states, initialized, repeats);
    const auto shared = Measure([&] {
        const auto status = omni::subop::SumRawGrouped(
            d.rows,
            [&](int32_t row) { return d.values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return !d.nulls.empty() && d.nulls[static_cast<size_t>(row)] != 0; },
            [&](int32_t row) { return d.statePtrs[static_cast<size_t>(row)]; },
            [&](int32_t row) { return initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] != 0; },
            [&](int32_t row) { initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] = 1; },
            nullptr);
        if (status != omni::subop::SUM_OK) std::exit(1);
    }, d.rows, states, initialized, repeats);
    std::printf("case=%s,native=%.3f,shared=%.3f\n", name.c_str(), native, shared);
}

void RunSingle(const std::string& name, Data d, int32_t repeats) {
    std::vector<int64_t> states(1, 0);
    std::vector<uint8_t> initialized(1, 0);
    const auto native = Measure([&] {
        bool has = false;
        int64_t value = 0;
        for (int32_t row = 0; row < d.rows; ++row) {
            const auto i = static_cast<size_t>(row);
            if ((d.active.empty() || d.active[i] != 0) &&
                (d.nulls.empty() || d.nulls[i] == 0)) {
                value = has ? static_cast<int64_t>(static_cast<uint64_t>(value) + static_cast<uint64_t>(d.values[i])) : d.values[i];
                has = true;
            }
        }
        if (has) states[0] = value;
    }, d.rows, states, initialized, repeats);
    const auto shared = Measure([&] {
        bool has = false;
        const auto status = omni::subop::SumRawSingleGroup(
            d.rows,
            [&](int32_t row) { return d.values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return !d.nulls.empty() && d.nulls[static_cast<size_t>(row)] != 0; },
            &states[0], has, &has, nullptr);
        if (status != omni::subop::SUM_OK) std::exit(1);
    }, d.rows, states, initialized, repeats);
    std::printf("case=%s,native=%.3f,shared=%.3f\n", name.c_str(), native, shared);
}
} // namespace

int main() {
    constexpr int32_t rows = 1000000;
    constexpr int32_t repeats = 7;
    std::printf("sum microbenchmark, rows=%d, repeats=%d\n", rows, repeats);
    RunRaw("raw_g8_dense", MakeData(rows, 8, 1, 0), repeats);
    RunRaw("raw_g64_dense", MakeData(rows, 64, 1, 0), repeats);
    RunRaw("raw_g64_sparse10", MakeData(rows, 64, 10, 0), repeats);
    RunRaw("raw_g64_null10", MakeData(rows, 64, 1, 10), repeats);
    RunSingle("single_dense", MakeData(rows, 1, 1, 0), repeats);
    RunSingle("single_sparse10", MakeData(rows, 1, 10, 0), repeats);
    return 0;
}
