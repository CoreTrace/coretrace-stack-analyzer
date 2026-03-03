#include <string>
#include <vector>

struct AnalysisResultLike
{
    std::vector<std::string> labels;
    std::vector<int> values;
};

struct PipelineStateLike
{
    int& token;
    AnalysisResultLike result;
};

AnalysisResultLike aggregate_ctor_member_copy_should_not_warn(int& token)
{
    PipelineStateLike state{token};
    return state.result;
}

// not contains: potential read of uninitialized local variable 'state'
// not contains: local variable 'state' is never initialized
