#include <string>

struct ConfigWithDefaultMember
{
    std::string path = "";
    int padded = 0;
};

int default_member_read_after_ctor(void)
{
    ConfigWithDefaultMember cfg;
    return cfg.padded;
}

// not contains: potential read of uninitialized local variable 'cfg'
// not contains: local variable 'cfg' is never initialized
