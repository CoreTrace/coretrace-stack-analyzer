// SPDX-License-Identifier: Apache-2.0
// #217 (D10): in C++ too, const IntPtr p becomes const int *const p, the pointer keeping its own
// const; a qualifier after the name is already kept.

// strict-expectation-details: true

// at line 27, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'before(int*)' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z6beforePi -> _Z6beforePKi): code compiled separately must use the updated declaration and be recompiled

// at line 28, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'after(int*)' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_Z5afterPi -> _Z5afterPKi): code compiled separately must use the updated declaration and be recompiled

// analyzer-arg: --experimental-const-suggestion-check

typedef int* IntPtr;

int before(const IntPtr p) { return *p; }
int after(IntPtr const p) { return *p; }
