// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>

namespace llvm
{
    class Function;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct IntRange
    {
        long long lower = 0;
        long long upper = 0;
        std::uint64_t hasLower : 1 = false;
        std::uint64_t hasUpper : 1 = false;
        std::uint64_t reservedFlags : 62 = 0;
    };

    std::map<const llvm::Value*, IntRange> computeIntRangesFromICmps(llvm::Function& F);
} // namespace ctrace::stack::analysis
