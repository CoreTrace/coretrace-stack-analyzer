// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class Module;
}

namespace ctrace::stack::analyzer
{

    class AnalysisPipeline
    {
      public:
        explicit AnalysisPipeline(const AnalysisConfig& config);

        AnalysisResult run(llvm::Module& mod) const;

      private:
        const AnalysisConfig& config_;
    };

} // namespace ctrace::stack::analyzer
