// SPDX-License-Identifier: Apache-2.0
// The resource-lifetime model: API contracts read from a text file.
//
//   acquire_out <pattern> <arg> <kind> [if_ret...]
//   acquire_ret <pattern> <kind>       [if_ret...]
//   release_arg <pattern> <arg> <kind> [if_ret...]
//
// The optional trailing qualifier makes the effect conditional on the call's
// return value: if_ret==0, if_ret!=0, if_ret>=0, if_ret<0, if_ret==null,
// if_ret!=null. Without it the effect is unconditional.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llvm
{
    class Function;
}

namespace ctrace::stack::analysis
{
    enum class RuleAction
    {
        AcquireOut,
        AcquireRet,
        ReleaseArg
    };

    enum class RuleCondition : std::uint8_t
    {
        Always,
        RetEqZero,
        RetNeZero,
        RetGeZero,
        RetLtZero,
        RetEqNull,
        RetNeNull
    };

    struct ResourceRule
    {
        std::string functionPattern;
        std::string resourceKind;
        unsigned argIndex = 0;
        RuleAction action = RuleAction::AcquireOut;
        RuleCondition condition = RuleCondition::Always;
        std::uint8_t reservedPadding[7] = {};
    };

    struct ResourceModel
    {
        std::vector<ResourceRule> rules;
    };

    bool parseResourceModel(const std::string& path, ResourceModel& out, std::string& error);

    /// Glob match of the rule's pattern against the callee's mangled and demangled names.
    bool ruleMatchesFunction(const ResourceRule& rule, const llvm::Function& callee);
} // namespace ctrace::stack::analysis
