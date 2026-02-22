#include <vector>

struct SupportStatusOut
{
    int capability = 0;
    std::vector<int> formats;
};

extern SupportStatusOut build_support_status_out_cross_tu();

int use_support_status_out_cross_tu()
{
    SupportStatusOut out = build_support_status_out_cross_tu();
    return out.capability;
}
