// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

struct Report
{
    std::vector<std::string> entries;
    std::vector<int> scores;
    bool finalized = false;
};

static Report build_report(int n)
{
    Report r;
    for (int i = 0; i < n; ++i)
        r.scores.push_back(i);
    return r;
}

int sret_complex_return_should_not_warn(void)
{
    Report rep = build_report(3);
    return static_cast<int>(rep.entries.size() + rep.scores.size());
}

// not contains: potential read of uninitialized local variable 'rep'
// not contains: potential read of uninitialized local variable 'r'
// not contains: local variable 'rep' is never initialized
// not contains: local variable 'r' is never initialized
