// SPDX-License-Identifier: Apache-2.0
static int read_leaf(const int* p)
{
    return *p;
}

static int read_mid(const int* p)
{
    return read_leaf(p);
}

int read_via_call_chain_before_init(void)
{
    int value;
    return read_mid(&value);
}

// at line 14, column 12
// [ !!Warn ] potential read of uninitialized local variable 'value'
// ↳ this call may read the value before any definite initialization in 'read_mid'
