// SPDX-License-Identifier: Apache-2.0
//
// Three pointers assigned in a ring from one buffer: one strongly connected component where only
// `p` points to the array directly, `q` through `p` and `r` through `q`. Every pointer must find
// the array, and the order of the accesses must not matter: `ring_pqr` writes through p, q and
// r, `ring_rqp` through r, q and p, and both report the same overflows with the same alias
// paths. The three writes also make MultipleStores report the buffer of each function.

// strict-expectation-details: true

void ring_pqr(int n)
{
    char buf[16] = {0};
    char* p = buf;
    char* q = p;
    char* r = q;
    if (n)
        p = r;

    // at line 26, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        p[i] = 1;

    // at line 34, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p -> q
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        q[i] = 2;

    // at line 42, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p -> q -> r
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        r[i] = 3;
}

void ring_rqp(int n)
{
    char buf[16] = {0};
    char* p = buf;
    char* q = p;
    char* r = q;
    if (n)
        p = r;

    // at line 60, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p -> q -> r
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        r[i] = 1;

    // at line 68, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p -> q
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        q[i] = 2;

    // at line 76, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
    //          ↳ alias path: buf -> arraydecay -> p
    //          ↳ index variable may go up to 19 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 20; ++i)
        p[i] = 3;
}

int main(void)
{
    ring_pqr(0);
    ring_rqp(1);
    return 0;
}

// MultipleStores sees the three writes of each function as stores into its buffer.
// at line 13, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (3 store instruction(s), 3 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping

// at line 47, column 1
// [ !Info! ] multiple stores to stack buffer 'buf' in this function (3 store instruction(s), 3 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping
