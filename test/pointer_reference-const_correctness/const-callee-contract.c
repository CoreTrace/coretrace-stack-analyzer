// SPDX-License-Identifier: Apache-2.0
// #167: a parameter only passed where a pointer to const is expected is not known to be
// unmodified. The callee's body is not checked, and this one casts the qualifier away.
// not contains: parameter 'p' in function 'use' is never used to modify the pointed object

static int bump(const int* p)
{
    *(int*)p += 1;
    return *p;
}

// at line 17, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'use' is only read, or passed where a pointer to const is expected
//          ↳ current type: int *p
//          ↳ suggested type: const int *p
//          ↳ the callee's body is not checked: it may still modify the pointed object through a cast
int use(int* p)
{
    return bump(p);
}

// Without a call, the function's own body establishes that the object is not modified.

// at line 28, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'q' in function 'peek' is never used to modify the pointed object
//          ↳ current type: int *q
//          ↳ suggested type: const int *q
int peek(int* q)
{
    return *q;
}
