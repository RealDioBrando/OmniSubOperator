/*
 * Copyright (c) 2026 OmniSubOperator contributors.
 *
 * Standalone count microbenchmark. This measures the adapter/kernel boundary,
 * not an end-to-end Spark query. It compares:
 *   - a direct native state loop,
 *   - the original normalized active/row-pointer API,
 *   - the grouped state-base API with a full-row active mask,
 *   - the grouped state-base API with compact selected rows.
 */

#include "kernel/count_batch.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Dataset {
    int32_t rows;
    int32_t groups;
    int32_t selectedCount;
    int32_t nullCount;
    std::vector<int32_t> groupIds;
    std::vector<uint8_t> active;
    std::vector<uint8_t> nulls;
    std::vector<int64_t*> rowCounts;
    std::vector<uint8_t*> rowBases;
    std::vector<int64_t> partial;
    std::vector<int64_t> compactPartial;
    std::vector<uint8_t*> compactBases;
    std::vector<uint8_t> compactNulls;
};

Dataset MakeDataset(int32_t rows, int32_t groups, int32_t selectedStride, int32_t nullStride) {
    Dataset data;
    data.rows = rows;
    data.groups = groups;
    data.groupIds.resize(static_cast<size_t>(rows));
    data.active.resize(static_cast<size_t>(rows), 1);
    data.nulls.resize(static_cast<size_t>(rows), 0);
    data.partial.resize(static_cast<size_t>(rows), 0);
    data.rowCounts.resize(static_cast<size_t>(rows), nullptr);
    data.rowBases.resize(static_cast<size_t>(rows), nullptr);

    const bool allSelected = selectedStride <= 1;
    if (allSelected) {
        data.active.clear();
    }
    for (int32_t row = 0; row < rows; ++row) {
        const auto index = static_cast<size_t>(row);
        data.groupIds[index] = row % groups;
        if (!allSelected) {
            data.active[index] = row % selectedStride == 0 ? 1 : 0;
        }
        if (nullStride > 0) {
            data.nulls[index] = row % nullStride == 0 ? 1 : 0;
        }
        data.partial[index] = 1 + row % 7;
    }
    if (nullStride <= 0) {
        data.nulls.clear();
    }

    data.selectedCount = 0;
    data.nullCount = 0;
    for (int32_t row = 0; row < rows; ++row) {
        const auto index = static_cast<size_t>(row);
        if (data.active.empty() || data.active[index] != 0) {
            ++data.selectedCount;
            data.compactBases.push_back(nullptr);
            data.compactPartial.push_back(data.partial[index]);
            const bool isNull = !data.nulls.empty() && data.nulls[index] != 0;
            data.compactNulls.push_back(isNull ? 1 : 0);
            if (isNull) {
                ++data.nullCount;
            }
        }
    }
    if (data.nullCount == 0) {
        data.compactNulls.clear();
    }

    for (int32_t row = 0; row < rows; ++row) {
        const auto index = static_cast<size_t>(row);
        data.rowBases[index] = reinterpret_cast<uint8_t*>(
            &data.groupIds[index]);
    }
    return data;
}

void PopulateStatePointers(Dataset& data, std::vector<int64_t>& states) {
    for (int32_t row = 0; row < data.rows; ++row) {
        const auto index = static_cast<size_t>(row);
        data.rowCounts[index] = &states[static_cast<size_t>(data.groupIds[index])];
        data.rowBases[index] = reinterpret_cast<uint8_t*>(
            &states[static_cast<size_t>(data.groupIds[index])]);
    }
    data.compactBases.clear();
    data.compactBases.reserve(static_cast<size_t>(data.selectedCount));
    for (int32_t row = 0; row < data.rows; ++row) {
        const auto index = static_cast<size_t>(row);
        if (data.active.empty() || data.active[index] != 0) {
            data.compactBases.push_back(reinterpret_cast<uint8_t*>(
                &states[static_cast<size_t>(data.groupIds[index])]));
        }
    }
}

OmniSubOpCountArgs MakeRawArgs(const Dataset& data) {
    OmniSubOpCountArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.rows;
    args.mode = OMNI_SUBOP_COUNT_ALL;
    args.encoding = OMNI_SUBOP_VALUE_FLAT;
    args.active_rows = data.active.empty() ? nullptr : data.active.data();
    args.nulls = data.nulls.empty() ? nullptr : data.nulls.data();
    args.row_counts = data.rowCounts.data();
    return args;
}

OmniSubOpCountArgs MakeRawExprArgs(const Dataset& data) {
    auto args = MakeRawArgs(data);
    args.mode = OMNI_SUBOP_COUNT_EXPR;
    return args;
}

OmniSubOpCountMergeArgs MakeMergeArgs(const Dataset& data) {
    OmniSubOpCountMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.rows;
    args.active_rows = data.active.empty() ? nullptr : data.active.data();
    args.nulls = data.nulls.empty() ? nullptr : data.nulls.data();
    args.partial_counts = data.partial.data();
    args.row_counts = data.rowCounts.data();
    return args;
}

OmniSubOpCountGroupedArgs MakeGroupedRawArgs(const Dataset& data) {
    OmniSubOpCountGroupedArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.rows;
    args.mode = OMNI_SUBOP_COUNT_ALL;
    args.active_rows = data.active.empty() ? nullptr : data.active.data();
    args.nulls = data.nulls.empty() ? nullptr : data.nulls.data();
    args.row_bases = data.rowBases.data();
    return args;
}

OmniSubOpCountGroupedArgs MakeGroupedRawExprArgs(const Dataset& data) {
    auto args = MakeGroupedRawArgs(data);
    args.mode = OMNI_SUBOP_COUNT_EXPR;
    return args;
}

OmniSubOpCountGroupedMergeArgs MakeGroupedMergeArgs(const Dataset& data) {
    OmniSubOpCountGroupedMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.rows;
    args.active_rows = data.active.empty() ? nullptr : data.active.data();
    args.nulls = data.nulls.empty() ? nullptr : data.nulls.data();
    args.partial_counts = data.partial.data();
    args.row_bases = data.rowBases.data();
    return args;
}

OmniSubOpCountGroupedArgs MakeCompactRawArgs(const Dataset& data, bool expr) {
    OmniSubOpCountGroupedArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.selectedCount;
    args.mode = expr ? OMNI_SUBOP_COUNT_EXPR : OMNI_SUBOP_COUNT_ALL;
    args.active_rows = nullptr;
    args.nulls = data.compactNulls.empty() ? nullptr : data.compactNulls.data();
    args.row_bases = data.compactBases.data();
    return args;
}

OmniSubOpCountGroupedMergeArgs MakeCompactMergeArgs(const Dataset& data) {
    OmniSubOpCountGroupedMergeArgs args{};
    args.abi_version = OMNI_SUBOP_ABI_VERSION;
    args.num_rows = data.selectedCount;
    args.active_rows = nullptr;
    args.nulls = data.compactNulls.empty() ? nullptr : data.compactNulls.data();
    args.partial_counts = data.compactPartial.data();
    args.row_bases = data.compactBases.data();
    return args;
}

template <typename Fn>
double MeasureNsPerRow(Fn&& fn, int32_t rows, std::vector<int64_t>& states, int32_t repeats) {
    volatile int64_t sink = 0;
    for (int32_t warmup = 0; warmup < 2; ++warmup) {
        std::fill(states.begin(), states.end(), 0);
        fn();
        sink += states[0];
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(repeats));
    for (int32_t repeat = 0; repeat < repeats; ++repeat) {
        std::fill(states.begin(), states.end(), 0);
        const auto begin = Clock::now();
        fn();
        const auto end = Clock::now();
        sink += states[0];
        const auto elapsed = std::chrono::duration<double, std::nano>(end - begin).count();
        samples.push_back(elapsed / static_cast<double>(rows));
    }
    std::sort(samples.begin(), samples.end());
    (void)sink;
    return samples[samples.size() / 2];
}

void RunRawCase(
    const std::string& name,
    int32_t rows,
    int32_t groups,
    int32_t selectedStride,
    int32_t nullStride,
    int32_t repeats) {
    Dataset data = MakeDataset(rows, groups, selectedStride, nullStride);
    std::vector<int64_t> states(static_cast<size_t>(groups), 0);
    PopulateStatePointers(data, states);
    const bool expr = nullStride > 0;
    const auto normalizedArgs = expr ? MakeRawExprArgs(data) : MakeRawArgs(data);
    const auto groupedArgs = expr ? MakeGroupedRawExprArgs(data) : MakeGroupedRawArgs(data);
    const auto compactArgs = MakeCompactRawArgs(data, expr);
    OmniSubOpCountResult result{};

    std::printf("case=%s,rows=%d,groups=%d,selected=%d,nulls=%d,path=", name.c_str(), rows, groups,
        data.selectedCount, data.nullCount);
    const auto native = MeasureNsPerRow([&] {
        for (int32_t row = 0; row < data.rows; ++row) {
            const auto index = static_cast<size_t>(row);
            if (data.active.empty() || data.active[index] != 0) {
                if (!expr || data.nulls.empty() || data.nulls[index] == 0) {
                    *reinterpret_cast<int64_t*>(data.rowBases[index]) += 1;
                }
            }
        }
    }, rows, states, repeats);
    std::printf("native=%.3f", native);

    const auto normalized = MeasureNsPerRow([&] {
        const int32_t status = expr ? omni_subop_count_update_raw(&normalizedArgs, &result)
                                    : omni_subop_count_update_raw(&normalizedArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "normalized raw status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",normalized=%.3f", normalized);

    const auto grouped = MeasureNsPerRow([&] {
        const int32_t status = expr ? omni_subop_count_update_grouped(&groupedArgs, &result)
                                    : omni_subop_count_update_grouped(&groupedArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "grouped raw status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",grouped=%.3f", grouped);

    const auto compact = MeasureNsPerRow([&] {
        const int32_t status = omni_subop_count_update_grouped(&compactArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "compact raw status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",compact=%.3f\n", compact);
}

void RunMergeCase(
    const std::string& name,
    int32_t rows,
    int32_t groups,
    int32_t selectedStride,
    int32_t nullStride,
    int32_t repeats) {
    Dataset data = MakeDataset(rows, groups, selectedStride, nullStride);
    std::vector<int64_t> states(static_cast<size_t>(groups), 0);
    PopulateStatePointers(data, states);
    const auto normalizedArgs = MakeMergeArgs(data);
    const auto groupedArgs = MakeGroupedMergeArgs(data);
    const auto compactArgs = MakeCompactMergeArgs(data);
    OmniSubOpCountResult result{};

    std::printf("case=%s,rows=%d,groups=%d,selected=%d,nulls=%d,path=", name.c_str(), rows, groups,
        data.selectedCount, data.nullCount);
    const auto native = MeasureNsPerRow([&] {
        for (int32_t row = 0; row < data.rows; ++row) {
            const auto index = static_cast<size_t>(row);
            if (data.active.empty() || data.active[index] != 0) {
                if (data.nulls.empty() || data.nulls[index] == 0) {
                    *reinterpret_cast<int64_t*>(data.rowBases[index]) += data.partial[index];
                }
            }
        }
    }, rows, states, repeats);
    std::printf("native=%.3f", native);

    const auto normalized = MeasureNsPerRow([&] {
        const int32_t status = omni_subop_count_merge_partial(&normalizedArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "normalized merge status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",normalized=%.3f", normalized);

    const auto grouped = MeasureNsPerRow([&] {
        const int32_t status = omni_subop_count_merge_grouped(&groupedArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "grouped merge status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",grouped=%.3f", grouped);

    const auto compact = MeasureNsPerRow([&] {
        const int32_t status = omni_subop_count_merge_grouped(&compactArgs, &result);
        if (status != OMNI_SUBOP_COUNT_OK) {
            std::fprintf(stderr, "compact merge status %d\n", status);
            std::exit(1);
        }
    }, rows, states, repeats);
    std::printf(",compact=%.3f\n", compact);
}

} // namespace

int main() {
    constexpr int32_t kRows = 1000000;
    constexpr int32_t kRepeats = 7;
    std::printf("count microbenchmark, rows=%d, repeats=%d\n", kRows, kRepeats);
    std::printf("values are median ns/row; lower is better\n");

    RunRawCase("raw_g1_dense", kRows, 1, 1, 0, kRepeats);
    RunRawCase("raw_g8_dense", kRows, 8, 1, 0, kRepeats);
    RunRawCase("raw_g64_dense", kRows, 64, 1, 0, kRepeats);
    RunRawCase("raw_g1024_dense", kRows, 1024, 1, 0, kRepeats);
    RunRawCase("raw_g64_sparse10", kRows, 64, 10, 0, kRepeats);
    RunRawCase("raw_g64_null10", kRows, 64, 1, 10, kRepeats);
    RunMergeCase("merge_g8_dense", kRows, 8, 1, 0, kRepeats);
    RunMergeCase("merge_g64_sparse10", kRows, 64, 10, 0, kRepeats);
    RunMergeCase("merge_g64_null10", kRows, 64, 1, 10, kRepeats);
    return 0;
}
