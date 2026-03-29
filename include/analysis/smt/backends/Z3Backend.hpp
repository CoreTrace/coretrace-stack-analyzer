// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "analysis/smt/ISmtBackend.hpp"

namespace ctrace::stack::analysis::smt
{
    class Z3Backend final : public ISmtBackend
    {
      public:
        Z3Backend() = default;

        std::string name() const override;
        SmtAnswer solve(const SmtQuery& query) const override;
    };
} // namespace ctrace::stack::analysis::smt
