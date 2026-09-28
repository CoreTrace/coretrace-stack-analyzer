// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <llvm/IR/Module.h>

#include <set>
#include <string>

namespace ctrace::stack
{
    void runFunctionAttrsPass(llvm::Module& mod);

    /// @brief Makes a call to a function that never returns end its path, as it does for a
    /// function declared `noreturn`.
    ///
    /// A function never returns when no `ret` is reachable from its entry once such calls end
    /// their paths: every path reaches `unreachable`, a `noreturn` call, a function that never
    /// returns, or loops forever. Mutually recursive functions are solved together. Those
    /// functions, and the declarations named in @p neverReturnElsewhere, are marked `noreturn`,
    /// and each call to a `noreturn` function is followed by `unreachable`, as clang emits for a
    /// function declared so.
    ///
    /// @param neverReturnElsewhere external functions that other modules found never to return.
    /// @return the external functions of @p mod that never return, for the other modules.
    std::set<std::string>
    endPathsAtCallsThatNeverReturn(llvm::Module& mod,
                                   const std::set<std::string>& neverReturnElsewhere = {});
} // namespace ctrace::stack
