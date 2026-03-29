// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

int g_zero[8];
int g_nonzero[4] = {1, 2, 3, 4};

static void sink_int(int value)
{
    printf("%d\n", value);
}

static void bad_global_read_before_write(void)
{
    int first = g_zero[0];
    for (int i = 0; i < 8; ++i)
        g_zero[i] = i;
    sink_int(first);
}
// at line 13, column 17
// [ !!Warn ] potential read of global buffer 'g_zero' before first write in this function
//         ↳ this buffer has static zero initialization; read is defined but may indicate stale/default-state use

static void good_global_write_before_read(void)
{
    g_zero[1] = 7;
    sink_int(g_zero[1]);
}

static void good_control_only_check(void)
{
    if (g_zero[2] == 0)
        g_zero[2] = 9;
}

static void good_nonzero_initializer(void)
{
    int value = g_nonzero[0];
    g_nonzero[0] = 5;
    sink_int(value);
}
// not contains: potential read of global buffer 'g_nonzero' before first write in this function

int main(void)
{
    bad_global_read_before_write();
    good_global_write_before_read();
    good_control_only_check();
    good_nonzero_initializer();
    return 0;
}
