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
    std::vector<uint64_t> activeMask;
    std::vector<uint64_t> nullMask;
    std::vector<int64_t> states;
    std::vector<uint8_t> initialized;
    std::vector<int64_t*> statePtrs;
};

Data MakeData(int32_t rows, int32_t groups, int32_t activeStride, int32_t nullStride) {
    Data data;
    data.rows = rows;
    data.groups = groups;
    data.groupIds.resize(static_cast<size_t>(rows));
    data.values.resize(static_cast<size_t>(rows));
    data.states.resize(static_cast<size_t>(groups), 0);
    data.initialized.assign(static_cast<size_t>(groups), 0);
    data.statePtrs.resize(static_cast<size_t>(rows), nullptr);
    const auto words = (static_cast<size_t>(rows) + 63) / 64;
    if (activeStride > 0) {
        data.activeMask.assign(words, 0);
    }
    if (nullStride > 0) {
        data.nullMask.assign(words, 0);
    }
    for (int32_t row = 0; row < rows; ++row) {
        const auto i = static_cast<size_t>(row);
        data.groupIds[i] = row % groups;
        data.values[i] = static_cast<int64_t>(row % 1000) - 500;
        if (activeStride > 0 && row % activeStride == 0) {
            data.activeMask[i / 64] |= 1ULL << (i % 64);
        }
        if (nullStride > 0 && row % nullStride == 0) {
            data.nullMask[i / 64] |= 1ULL << (i % 64);
        }
        data.statePtrs[i] = &data.states[static_cast<size_t>(data.groupIds[i])];
    }
    return data;
}

void Reset(Data& data) {
    std::fill(data.states.begin(), data.states.end(), 0);
    std::fill(data.initialized.begin(), data.initialized.end(), 0);
}

template <typename Fn>
double Measure(Fn&& fn, Data& data, int32_t repeats) {
    volatile int64_t sink = 0;
    for (int32_t warmup = 0; warmup < 2; ++warmup) {
        Reset(data);
        fn(data);
        sink += data.states[0];
    }
    std::vector<double> samples;
    for (int32_t repeat = 0; repeat < repeats; ++repeat) {
        Reset(data);
        const auto begin = Clock::now();
        fn(data);
        const auto end = Clock::now();
        sink += data.states[0];
        samples.push_back(
            std::chrono::duration<double, std::nano>(end - begin).count() /
            data.rows);
    }
    std::sort(samples.begin(), samples.end());
    (void)sink;
    return samples[samples.size() / 2];
}

bool Active(const Data& data, int32_t row) {
    return data.activeMask.empty() ||
        ((data.activeMask[static_cast<size_t>(row) / 64] >>
          (static_cast<size_t>(row) % 64)) & 1ULL) != 0;
}

bool Null(const Data& data, int32_t row) {
    return !data.nullMask.empty() &&
        ((data.nullMask[static_cast<size_t>(row) / 64] >>
          (static_cast<size_t>(row) % 64)) & 1ULL) != 0;
}

omni::subop::Int64BatchView MakeView(Data& data) {
    omni::subop::Int64BatchView view;
    view.num_rows = data.rows;
    view.row_begin = 0;
    view.row_end = data.rows;
    view.value_count = data.rows;
    view.values = data.values.data();
    view.state_values = data.statePtrs.data();
    view.active_mask = data.activeMask.empty() ? nullptr : data.activeMask.data();
    view.null_mask = data.nullMask.empty() ? nullptr : data.nullMask.data();
    view.trusted = true;
    return view;
}

void Run(const std::string& name, const Data& input, int32_t repeats) {
    Data data = input;
    const auto initializedAt = [&](Data& d, int32_t row) {
        return d.initialized[static_cast<size_t>(
            d.groupIds[static_cast<size_t>(row)])] != 0;
    };
    const auto markAt = [&](Data& d, int32_t row) {
        d.initialized[static_cast<size_t>(
            d.groupIds[static_cast<size_t>(row)])] = 1;
    };

    const auto native = Measure([&](Data& d) {
        for (int32_t row = 0; row < d.rows; ++row) {
            if (!Active(d, row) || Null(d, row)) {
                continue;
            }
            const auto group = static_cast<size_t>(
                d.groupIds[static_cast<size_t>(row)]);
            const auto value = d.values[static_cast<size_t>(row)];
            if (!d.initialized[group]) {
                d.states[group] = value;
                d.initialized[group] = 1;
            } else if (value < d.states[group]) {
                d.states[group] = value;
            }
        }
    }, data, repeats);

    const auto shared = Measure([&](Data& d) {
        const auto status = omni::subop::MinMaxRawGroupedDirect(
            d.rows,
            [&](int32_t row) { return d.values[static_cast<size_t>(row)]; },
            [&](int32_t row) { return !Active(d, row) || Null(d, row); },
            [&](int32_t row) { return d.statePtrs[static_cast<size_t>(row)]; },
            [&](int32_t row) { return initializedAt(d, row); },
            [&](int32_t row) { markAt(d, row); },
            omni::subop::MIN_MAX_MIN,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, data, repeats);

    auto view = MakeView(data);
    const auto packedScalar = Measure([&](Data& d) {
        const auto status = omni::subop::MinMaxRawPacked(
            view,
            [&](int32_t row) { return initializedAt(d, row); },
            [&](int32_t row) { markAt(d, row); },
            omni::subop::MIN_MAX_MIN,
            omni::subop::MIN_MAX_SCALAR,
            nullptr);
        if (status != omni::subop::SUM_OK) {
            std::exit(1);
        }
    }, data, repeats);

    std::printf(
        "case=%s,native=%.3f,shared_callback=%.3f,packed_scalar=%.3f",
        name.c_str(),
        native,
        shared,
        packedScalar);
#if defined(__ARM_FEATURE_SVE)
    if (omni::subop::sum_detail::CanUsePackedSve()) {
        const auto packedSve = Measure([&](Data& d) {
            const auto status = omni::subop::MinMaxRawPacked(
                view,
                [&](int32_t row) { return initializedAt(d, row); },
                [&](int32_t row) { markAt(d, row); },
                omni::subop::MIN_MAX_MIN,
                omni::subop::MIN_MAX_SVE,
                nullptr);
            if (status != omni::subop::SUM_OK) {
                std::exit(1);
            }
        }, data, repeats);
        std::printf(",packed_sve=%.3f\n", packedSve);
    } else {
        std::printf(",packed_sve=unsupported\n");
    }
#else
    std::printf(",packed_sve=unsupported\n");
#endif
}
} // namespace

int main() {
    constexpr int32_t rows = 1000000;
    constexpr int32_t repeats = 7;
    std::printf("min/max microbenchmark, rows=%d, repeats=%d\n", rows, repeats);
    Run("min_g64_dense", MakeData(rows, 64, 0, 0), repeats);
    Run("min_g64_null10", MakeData(rows, 64, 0, 10), repeats);
    Run("min_g64_active10", MakeData(rows, 64, 10, 0), repeats);
    return 0;
}