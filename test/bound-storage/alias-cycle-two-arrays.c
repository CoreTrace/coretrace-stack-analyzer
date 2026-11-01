// SPDX-License-Identifier: Apache-2.0
//
// Pointers assigned from each other under a condition form one strongly connected component.
// When the component reaches two arrays of different sizes, the resolver reports no array for
// it, as it does for any pointer that may point into several arrays, so neither write through
// `p` or `q` is reported even though both arrays overflow at index 39. This is a deliberate
// trade-off, not a gain in detection: the old resolver reported each pointer against its own
// array, which depended on the order of the walk. The control case `same_array` has the same
// shape with both pointers from one array, and its overflow must be reported.

// strict-expectation-details: true
// not contains: overflow on variable 'a' (size 16)
// not contains: overflow on variable 'b' (size 32)

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

void same_array(int n)
{
    char c[16] = {0};
    char* p = c;
    char* q = c;
    if (n)
        p = q;
    else
        q = p;
    p[0] = 0;

    // at line 62, column 14
    // [ !!Warn ] potential stack buffer overflow on variable 'c' (size 16)
    //          ↳ alias path: c -> arraydecay1 -> q
    //          ↳ index variable may go up to 39 (array last valid index: 15)
    //          ↳ (this is a write access)
    for (int i = 0; i < 40; ++i)
        q[i] = 1;
}

int main(void)
{
    both_arrays(0);
    both_arrays_reverse_query(1);
    same_array(0);
    return 0;
}

// In the control case, MultipleStores sees both writes as stores into the one array.
// at line 47, column 1
// [ !Info! ] multiple stores to stack buffer 'c' in this function (2 store instruction(s), 2 distinct index expression(s))
// [ !Info! ] stores use different index expressions; verify indices are correct and non-overlapping
