#include <vector>

struct SupportStatusOut
{
    int capability = 0;
    std::vector<int> formats;
};

extern int ext_status_fill(int* out);

SupportStatusOut build_support_status_out_cross_tu()
{
    SupportStatusOut details;
    (void)ext_status_fill(&details.capability);
    if (details.capability != 0)
        details.formats.push_back(1);
    return details;
}
