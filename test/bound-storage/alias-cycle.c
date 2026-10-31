// SPDX-License-Identifier: Apache-2.0
//
// Two pointers assigned from each other. Resolving `p` reaches `q`, which reaches `p` again
// while `p` is still being resolved. A result remembered for `q` at that moment would say
// "nothing", and the write through `q` would go unreported (#215, first review).

void cycle(int n)
{
    char buf[16] = {0};
    char* p = buf;
    char* q = p;
    p = q;
    p[0] = n;

    // at line 21, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p -> q
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        q[i] = 1;
}

int main(void)
{
    cycle(3);
    return 0;
}
