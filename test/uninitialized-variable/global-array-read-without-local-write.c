// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

int g_zero_cross_state[8];
int g_zero_local_ok[4];

static void external_write(void)
{
    g_zero_cross_state[0] = 42;
}

static int read_without_local_write(void)
{
    return g_zero_cross_state[0];
}
// at line 13, column 12
// [ !!Warn ] potential read of global buffer 'g_zero_cross_state' before first write in this function
//         ↳ no write to this buffer is observed in this function; value may come from static initialization or writes in other functions/TUs

static int read_after_local_write(void)
{
    g_zero_local_ok[1] = 7;
    return g_zero_local_ok[1];
}
// not contains: potential read of global buffer 'g_zero_local_ok' before first write in this function

int main(void)
{
    external_write();
    const int a = read_without_local_write();
    const int b = read_after_local_write();
    printf("%d %d\\n", a, b);
    return 0;
}
