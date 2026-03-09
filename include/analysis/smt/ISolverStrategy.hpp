#pragma once

#include "analysis/smt/ISmtBackend.hpp"

#include <memory>
#include <vector>

namespace ctrace::stack::analysis::smt
{
    class ISolverStrategy
    {
      public:
        virtual ~ISolverStrategy() = default;
        virtual std::vector<SmtAnswer>
        run(const SmtQuery& query,
            const std::vector<std::shared_ptr<ISmtBackend>>& backends) const = 0;
    };
} // namespace ctrace::stack::analysis::smt
