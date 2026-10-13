// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

namespace ctrace::stack::analysis::const_suggestion
{
    // The demangled form of a symbol without const and spaces, with && written &: the rewritten
    // function, and what is named inside it, keep this form across the rewrite.
    std::string withoutConst(const std::string& symbol);

    // The constructor or destructor variant a symbol names, and whether it names a const method.
    std::string variantOf(const std::string& symbol);
} // namespace ctrace::stack::analysis::const_suggestion
