// SPDX-License-Identifier: Apache-2.0
// Module B of the incomplete-summary unit test (#157), built with the normal budget and the index
// of module A. gv() is incomplete through the imported summary of fi(), and gv2() through the
// summary of gv() in this module; h() uses nothing and is complete. use_f() and use_fi() read v
// after a call whose summary is incomplete: they must report what an unknown declaration reports.

void f(int* p);
int fi(int* p);

int use_f(void)
{
    int v;
    f(&v);
    return v;
}

int use_fi(void)
{
    int v;
    fi(&v);
    return v;
}

void gv(int* p)
{
    fi(p);
}

void gv2(int* p)
{
    gv(p);
}

void h(int* p)
{
    (void)p;
}
