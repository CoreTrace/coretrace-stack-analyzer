// SPDX-License-Identifier: Apache-2.0
// Module A of the incomplete-summary unit test (#157). It is built with a one-iteration limit, at
// which none of these analyses converges: f() and fi() write nothing, but their summaries are
// incomplete.

void f(int* p)
{
    (void)p;
}

int fi(int* p)
{
    (void)p;
    return 0;
}
