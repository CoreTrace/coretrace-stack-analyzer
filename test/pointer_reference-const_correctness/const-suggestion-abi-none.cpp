// SPDX-License-Identifier: Apache-2.0
// #217: a function whose symbol does not change gets no ABI note: internal linkage, or a C name.

// strict-expectation-details: true
// not contains: ↳ ABI:

// at line 12, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'internal_read(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
static int internal_read(int* p)
{
    return *p;
}

namespace
{
    // at line 24, column 0
    // [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function '(anonymous namespace)::anonymous_read(int*)' is never used to modify the pointed object
    // ↳ current type: int *p
    // ↳ suggested type: const int *p
    // ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
    int anonymous_read(int* p)
    {
        return *p;
    }
} // namespace

// at line 35, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'c_read' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
extern "C" int c_read(int* p)
{
    return *p;
}

int use_internal_reads()
{
    int value = 1;
    return internal_read(&value) + anonymous_read(&value);
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
