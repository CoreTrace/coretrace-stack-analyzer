// SPDX-License-Identifier: Apache-2.0
static int read_uninitialized_value(void)
{
    int value;
    return value;
}

static int clean_value(void)
{
    return 7;
}

int main(void)
{
    return read_uninitialized_value() + clean_value();
}

// at line 4, column 12
// [ !!Warn ] potential read of uninitialized local variable 'value'
//          ↳ this load may execute before any definite initialization on all control-flow paths
