// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>

struct Config
{
    int mode = 0;
    bool enabled = false;
    std::vector<std::string> names = {};
    std::string path = "";
};

int copy_default_initialized_config(void)
{
    Config cfg;
    Config out;
    out = cfg;
    return static_cast<int>(out.names.size());
}

// not contains: potential read of uninitialized local variable 'cfg'
// not contains: potential read of uninitialized local variable 'out'
