// SPDX-License-Identifier: Apache-2.0
// #217: a macro that writes several parameters cannot take `const` before its invocation: the
// qualifier would go to the first parameter it writes, n, not to q. The suggestion is kept as not
// checked.

// strict-expectation-details: true

// at line 17, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'q' in function 'read_both' is never used to modify the pointed object
// ↳ current type: int *q
// ↳ suggested type: const int *q
// ↳ not checked with Clang: the parameter's type is written by a macro that writes other parameters too

// analyzer-arg: --experimental-const-suggestion-check
#define READ_ARGS int n, int* q

int read_both(READ_ARGS)
{
    n = (*q != 0);
    return n;
}
