// SPDX-License-Identifier: Apache-2.0
// A static homonym of cross-tu-const-suggestion-static-read.c's helper, which writes through its
// parameter.
static int helper(int* p)
{
    *p = 2;
    return 0;
}

int write_through_helper(void)
{
    int value = 1;
    return helper(&value);
}
