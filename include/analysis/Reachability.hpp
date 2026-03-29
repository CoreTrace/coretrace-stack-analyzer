// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "analysis/StackBufferAnalysis.hpp"

namespace ctrace::stack::analysis
{

    bool isStaticallyUnreachableStackAccess(const StackBufferOverflowIssue& issue);

} // namespace ctrace::stack::analysis
