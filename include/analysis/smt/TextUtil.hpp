#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace ctrace::stack::analysis::smt
{
    inline std::string toLowerAscii(std::string_view input)
    {
        std::string out;
        out.reserve(input.size());
        for (char c : input)
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        return out;
    }
} // namespace ctrace::stack::analysis::smt
