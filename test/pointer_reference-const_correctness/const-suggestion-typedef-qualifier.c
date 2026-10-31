// SPDX-License-Identifier: Apache-2.0
// #217 (D10): a pointer typedef with a qualifier written apart. The rewrite writes the candidate
// in place of the whole written type, so that the pointer keeps its own const: const IntPtr p
// becomes const int *const p. A qualifier after the name is already kept. A declaration and the
// definition that differ in the qualifier each keep their own. A qualifier written by a macro
// cannot be folded into the candidate: the suggestion is kept, not checked.

// strict-expectation-details: true

// at line 43, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'before' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 47, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'after' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 52, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'declared' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 56, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'by_macro' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ not checked with Clang: the qualifier of the parameter's typedef is written by a macro

// analyzer-arg: --experimental-const-suggestion-check

typedef int* IntPtr;
#define CONST const

int before(const IntPtr p)
{
    return *p;
}
int after(IntPtr const p)
{
    return *p;
}
int declared(IntPtr p);
int declared(const IntPtr p)
{
    return *p;
}
int by_macro(CONST IntPtr p)
{
    return *p;
}
