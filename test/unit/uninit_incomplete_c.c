// SPDX-License-Identifier: Apache-2.0
// Module C of the incomplete-summary unit test (#157), built with the normal budget and the
// indexes of modules A and B. The summary of gv2() is empty but incomplete: use_gv2() must report
// what an unknown declaration reports, not a read after a call that writes nothing.

void gv2(int* p);

int use_gv2(void)
{
    int v;
    gv2(&v);
    return v;
}
