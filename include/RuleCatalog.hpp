// SPDX-License-Identifier: Apache-2.0
// RuleCatalog.hpp
#pragma once

#include <optional>
#include <string_view>

namespace ctrace::stack
{
    /// What a rule detects, as the reports describe it: a title, and a sentence on what its
    /// findings mean. GitHub code scanning shows them to explain an alert.
    struct RuleDescription
    {
        std::string_view title;
        std::string_view description;
    };

    /// The description of the rule @p ruleId, the id a Diagnostic carries; nothing for an id
    /// the analyzer does not report. The SARIF report reads the same text.
    [[nodiscard]] std::optional<RuleDescription> describeRule(std::string_view ruleId);
} // namespace ctrace::stack
