#pragma once

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
        bool hasLower = false;
        long long lower = 0;
        bool hasUpper = false;
        long long upper = 0;
    };

    std::map<const llvm::Value*, IntRange> computeIntRangesFromICmps(llvm::Function& F);
} // namespace ctrace::stack::analysis
