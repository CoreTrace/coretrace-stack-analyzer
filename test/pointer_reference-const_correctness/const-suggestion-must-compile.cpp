// SPDX-License-Identifier: Apache-2.0
// #217: a parameter is reported only if the analyzed files still compile with its suggested type.

// strict-expectation-details: true
// not contains: parameter 'p' in function 'address_to_int_pp(int*)'
// not contains: parameter 'value' in function 'unused_lambda(int*)'
// not contains: parameter 'r' in function 'keep(int&)'

// &p is passed where int** is expected: no function matches the const version.
static int read_pp(int** pp)
{
    return **pp;
}

int address_to_int_pp(int* p)
{
    return read_pp(&p);
}

// A writing lambda that is never called: its body is not in the generated code.
int unused_lambda(int* value)
{
    auto bump = [&]() { return ++*value; };
    (void)bump;
    return 0;
}

// A reference bound to an int& member of an aggregate.
struct Root
{
    int& ref;
};

void keep(int& r)
{
    Root root{r};
    (void)root;
}

// Control: nothing depends on the parameter's type.
// at line 47, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z10read_valuePi -> _Z10read_valuePKi): code compiled separately must use the updated declaration and be recompiled
int read_value(int* p)
{
    return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
