// SPDX-License-Identifier: Apache-2.0
#include <vector>

struct AnalysisResultLike
{
    unsigned warningCount = 0;
    unsigned errorCount = 0;
};

static AnalysisResultLike mergeResultsLike(const std::vector<AnalysisResultLike>& results)
{
    AnalysisResultLike merged{};
    for (const AnalysisResultLike& item : results)
    {
        merged.warningCount += item.warningCount;
        merged.errorCount += item.errorCount;
    }
    return merged;
}

unsigned fp_uninitialized_merged_return(const std::vector<AnalysisResultLike>& results)
{
    AnalysisResultLike merged = mergeResultsLike(results);
    return merged.warningCount + merged.errorCount;
}

// strict-diagnostic-count: false
// not contains: potential read of uninitialized local variable 'merged'
