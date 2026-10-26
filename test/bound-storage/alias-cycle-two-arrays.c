// SPDX-License-Identifier: Apache-2.0
//
// Two pointers to two arrays of different sizes, assigned from each other under a condition:
// one strongly connected component that reaches two arrays. The resolver reports no array for
// it, so neither write is reported, even though both arrays overflow at index 39. The old
// resolver reported each pointer against its own array; the component's result now depends
// on the function alone, and several arrays mean no array (#215, second review).

// not contains: potential stack buffer overflow

void both_arrays(int n)
{
    char a[16] = {0};
    char b[32] = {0};
    char* p = a;
    char* q = b;
    if (n)
        p = q;
    else
        q = p;
    p[0] = 0;
    for (int i = 0; i < 40; ++i)
        q[i] = 1;
}

void both_arrays_reverse_query(int n)
{
    char a[16] = {0};
    char b[32] = {0};
    char* p = a;
    char* q = b;
    if (n)
        p = q;
    else
        q = p;
    q[0] = 0;
    for (int i = 0; i < 40; ++i)
        p[i] = 1;
}

int main(void)
{
    both_arrays(0);
    both_arrays_reverse_query(1);
    return 0;
}
