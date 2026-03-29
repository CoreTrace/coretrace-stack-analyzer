// SPDX-License-Identifier: Apache-2.0
struct BitfieldConfig
{
    unsigned mode : 3 = 0;
    unsigned enabled : 1 = 0;
    unsigned reserved : 28 = 0;
};

int bitfield_default_member_init_should_not_warn(void)
{
    BitfieldConfig cfg;
    return static_cast<int>(cfg.mode);
}

// not contains: potential read of uninitialized local variable 'cfg'
