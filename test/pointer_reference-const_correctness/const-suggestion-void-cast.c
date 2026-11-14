// SPDX-License-Identifier: Apache-2.0
// #217: `(void)read_value` converts the function to no other function type: the suggestion is
// checked and kept. An explicit cast to another function type keeps it, not checked
// (cross-tu-const-suggestion-cast.c).

// strict-expectation-details: true

// at line 16, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// analyzer-arg: --experimental-const-suggestion-check

static int read_value(int* p)
{
    (void)read_value;
    return *p;
}

int use_value(void)
{
    int value = 1;
    return read_value(&value);
}
