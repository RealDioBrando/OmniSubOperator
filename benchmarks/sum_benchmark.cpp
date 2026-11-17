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
    std::vector<uint64_t> activeMask;
    std::vector<uint64_t> nullMask;
    std::vector<int64_t*> statePtrs;
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
    const size_t wordCount = (static_cast<size_t>(rows) + 63) / 64;
    d.activeMask.assign(wordCount, 0);
    d.nullMask.assign(wordCount, 0);
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
        if (d.active[i] != 0) {
            d.activeMask[i / 64] |= 1ULL << (i % 64);
            ++d.selected;
        }
        if (d.nulls[i] != 0) {
            d.nullMask[i / 64] |= 1ULL << (i % 64);
        }
    }
    if (stride <= 1) {
        d.active.clear();
        d.activeMask.clear();
    }
    if (nullStride <= 0) {
        d.nulls.clear();
        d.nullMask.clear();
    }
    return d;
}

void FillStatePointers(Data& d, std::vector<int64_t>& states) {
    for (int32_t row = 0; row < d.rows; ++row) {
        const auto i = static_cast<size_t>(row);
        d.statePtrs[i] = &states[static_cast<size_t>(d.groupIds[i])];
    }
}

omni::subop::SumInt64BatchView MakeView(Data& d) {
    omni::subop::SumInt64BatchView view;
    view.num_rows = d.rows;
    view.row_begin = 0;
    view.row_end = d.rows;
    view.value_count = d.rows;
    view.value_layout = omni::subop::SUM_VALUE_FLAT;
    view.values = d.values.data();
    view.state_values = d.statePtrs.data();
    view.active_mask = d.activeMask.empty() ? nullptr : d.activeMask.data();
    view.null_mask = d.nullMask.empty() ? nullptr : d.nullMask.data();
    view.trusted = true;
    return view;
}

template <typename Fn>
double Measure(
    Fn&& fn,
    int32_t rows,
    std::vector<int64_t>& states,
    std::vector<uint8_t>& init,
    int32_t repeats) {
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
    auto view = MakeView(d);

    const auto native = Measure([&] {
        for (int32_t row = 0; row < d.rows; ++row) {
            const auto i = static_cast<size_t>(row);
            if (!d.active.empty() && d.active[i] == 0) {
                continue;
            }
            if (!d.nulls.empty() && d.nulls[i] != 0) {
                continue;
            }
            const auto group = static_cast<size_t>(d.groupIds[i]);
            auto& state = *d.statePtrs[i];
            state = initialized[group]
                ? static_cast<int64_t>(
                      static_cast<uint64_t>(state) + static_cast<uint64_t>(d.values[i]))
                : d.values[i];
            initialized[group] = 1;
        }
    }, d.rows, states, initialized, repeats);

    const auto generic = Measure([&] {
        const auto status = omni::subop::SumRawGrouped(
            d.rows,
            [&](int32_t row) { return d.values[static_cast<size_t>(row)]; },
            [&](int32_t row) {
                return !d.nulls.empty() && d.nulls[static_cast<size_t>(row)] != 0;
            },
            [&](int32_t row) { return d.statePtrs[static_cast<size_t>(row)]; },
            [&](int32_t row) {
                return initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] != 0;
            },
            [&](int32_t row) {
                initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] = 1;
            },
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, d.rows, states, initialized, repeats);

    const auto packedScalar = Measure([&] {
        const auto status = omni::subop::SumRawPacked(
            view,
            [&](int32_t row) {
                return initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] != 0;
            },
            [&](int32_t row) {
                initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] = 1;
            },
            omni::subop::SUM_EXECUTION_SCALAR,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, d.rows, states, initialized, repeats);

    double packedSve = -1.0;
#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        packedSve = Measure([&] {
            const auto status = omni::subop::SumRawPacked(
                view,
                [&](int32_t row) {
                    return initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] != 0;
                },
                [&](int32_t row) {
                    initialized[static_cast<size_t>(d.groupIds[static_cast<size_t>(row)])] = 1;
                },
                omni::subop::SUM_EXECUTION_SVE,
                nullptr);
            if (status != omni::subop::SUM_OK) {
                std::exit(1);
            }
        }, d.rows, states, initialized, repeats);
    }
#endif

    std::printf(
        "case=%s,native=%.3f,generic=%.3f,packed_scalar=%.3f",
        name.c_str(),
        native,
        generic,
        packedScalar);
    if (packedSve >= 0.0) {
        std::printf(",packed_sve=%.3f\n", packedSve);
    } else {
        std::printf(",packed_sve=unsupported\n");
    }
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
                value = has
                    ? static_cast<int64_t>(
                          static_cast<uint64_t>(value) + static_cast<uint64_t>(d.values[i]))
                    : d.values[i];
                has = true;
            }
        }
        if (has) {
            states[0] = value;
        }
    }, d.rows, states, initialized, repeats);
    const auto shared = Measure([&] {
        bool has = false;
        const auto status = omni::subop::SumRawSingleGroup(
            d.rows,
            [&](int32_t row) { return d.values[static_cast<size_t>(row)]; },
            [&](int32_t row) {
                return !d.nulls.empty() && d.nulls[static_cast<size_t>(row)] != 0;
            },
            &states[0],
            has,
            &has,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
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
    RunRaw("raw_unique", MakeData(rows, rows, 1, 0), repeats);
    RunSingle("single_dense", MakeData(rows, 1, 1, 0), repeats);
    RunSingle("single_sparse10", MakeData(rows, 1, 10, 0), repeats);
    return 0;
}