// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace llvm
{
    class CallBase;
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    /// A load, a store or a memory intrinsic that a function makes through a pointer parameter,
    /// at a constant offset from the address the parameter holds on entry, on every run of the
    /// function that returns (#266). The location is the access itself, at the end of the calls.
    struct ParamAccess
    {
        std::int64_t offset = 0; // bytes from the parameter's address
        std::uint64_t size = 0;  // bytes
        std::string function;    // linker symbol of the function that makes the access
        std::string file;
        unsigned line = 0;
        unsigned column = 0;
    };

    /// The accesses through one parameter that reach furthest each way: the lowest offset and
    /// the highest end, for reads and for writes.
    struct ParamAccessBounds
    {
        std::optional<ParamAccess> lowestRead;
        std::optional<ParamAccess> highestRead;
        std::optional<ParamAccess> lowestWrite;
        std::optional<ParamAccess> highestWrite;
    };

    /// What a function is known to access through its pointer parameters, one entry per
    /// parameter.
    ///
    /// A summary lists accesses that happen. It never lists all of them: an access on some runs
    /// only, at a variable offset, or through a pointer the walk loses is left out. No summary,
    /// complete or not, shows that a call is safe.
    ///
    /// complete is false when a budget stopped the computation (offsets too large, rounds over
    /// a cycle of calls), or when the summary used an incomplete one. Its accesses still happen.
    struct ParamAccessSummary
    {
        std::vector<ParamAccessBounds> params;
        std::uint64_t complete : 1 = true;
        std::uint64_t reservedFlags : 63 = 0;
    };

    /// Summaries keyed by linker symbol, for calls from other modules.
    struct ParamAccessIndex
    {
        std::map<std::string, ParamAccessSummary> functions;
    };

    using ParamAccessSummaries = std::map<const llvm::Function*, ParamAccessSummary>;

    /// The summaries of the functions @p mod defines. A call to a declaration takes the summary
    /// @p elsewhere has for its symbol, if it has one parameter per argument.
    ParamAccessSummaries computeParamAccessSummaries(llvm::Module& mod,
                                                     const ParamAccessIndex* elsewhere);

    /// The summary a call reaches: from @p summaries for a function of this module, from
    /// @p elsewhere for a declaration. Null for an indirect call or a function without one.
    const ParamAccessSummary* calleeParamAccesses(const llvm::CallBase& call,
                                                  const ParamAccessSummaries& summaries,
                                                  const ParamAccessIndex* elsewhere);

    /// The summaries of the external functions defined across @p modules, for each module to
    /// use for the others. Rounds repeat until one changes nothing, or until a budget of rounds
    /// stops them and marks every summary incomplete. A symbol with more than one definition, or
    /// one that may be replaced at link time, gets none.
    ParamAccessIndex paramAccessesAcrossModules(const std::vector<llvm::Module*>& modules);
} // namespace ctrace::stack::analysis
