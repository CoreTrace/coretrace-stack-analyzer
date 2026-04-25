// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <llvm/Demangle/Demangle.h>

namespace ctrace_tools
{
    template <typename T> concept StringLike = std::convertible_to<T, std::string_view>;

    [[nodiscard]] inline bool isMangled(StringLike auto name) noexcept
    {
        const std::string_view sv{name};
        if (sv.empty())
        {
            return false;
        }

        // Itanium names usually start with _Z, while MSVC names commonly
        // start with ? or ??. Use a cheap prefix check before calling into
        // LLVM's demangler.
        const bool looksMangled =
            sv.starts_with("_Z") || sv.starts_with("?") || sv.starts_with(".?");
        if (!looksMangled)
        {
            return false;
        }

        return llvm::demangle(sv) != sv;
    }

    [[nodiscard]] std::string mangleFunction(const std::string& namespaceName,
                                             const std::string& functionName,
                                             const std::vector<std::string>& paramTypes);

    [[nodiscard]] std::string demangle(const char* name);

    [[nodiscard]] std::string canonicalizeMangledName(std::string_view name);
} // namespace ctrace_tools
