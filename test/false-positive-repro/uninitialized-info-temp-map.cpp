// SPDX-License-Identifier: Apache-2.0
#include <map>
#include <vector>

struct LocalInfoLike
{
    unsigned long long bytes = 0;
    bool unknown = false;
};

static LocalInfoLike computeInfoLike(int seed)
{
    LocalInfoLike info{};
    info.bytes = static_cast<unsigned long long>(seed >= 0 ? seed : -seed);
    info.unknown = (seed % 2) == 0;
    return info;
}

unsigned long long fp_uninitialized_info_temp_map(const std::vector<int>& values)
{
    std::map<int, LocalInfoLike> localById;
    for (int value : values)
    {
        LocalInfoLike info = computeInfoLike(value);
        localById[value] = info;
    }

    unsigned long long sum = 0;
    for (const auto& [key, info] : localById)
        sum +=
            info.bytes + (info.unknown ? 1ull : 0ull) + static_cast<unsigned long long>(key >= 0);
    return sum;
}

// strict-diagnostic-count: false
// not contains: potential read of uninitialized local variable 'info'
