// SPDX-License-Identifier: Apache-2.0
//
// A cycle that goes through PHIs of pointers: `q` takes a conditional operator over `p` and the
// buffer, and `p` is reassigned in a loop from a conditional operator over `q` and itself. The
// component holds the two pointer variables and the two PHIs, and the alias path of `q` goes
// through a PHI (`cond`). Both writes must be reported and MultipleStores must see both stores.

// strict-expectation-details: true

void phi_cycle(int n)
{
    char buf[16] = {0};
    char* p = buf;
    char* q = (n & 1) ? p : buf;
    for (int i = 0; i < n; ++i)
        p = (i & 1) ? q : p;

    // at line 24, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        p[i] = 1;

    // at line 32, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay1 -> cond -> q
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        q[i] = 2;
}

int main(void)
{
    phi_cycle(3);
    return 0;
}

// MultipleStores sees both writes as stores into the buffer.
// at line 12, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (2 store instruction(s), 2 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping
