#pragma once

#include "analysis/smt/SolverTypes.hpp"

#include <string>

namespace ctrace::stack::analysis::smt
{
    class ISmtBackend
    {
      public:
        virtual ~ISmtBackend() = default;
        virtual std::string name() const = 0;
        virtual SmtAnswer solve(const SmtQuery& query) const = 0;
    };
} // namespace ctrace::stack::analysis::smt
