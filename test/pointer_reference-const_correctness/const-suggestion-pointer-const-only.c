// SPDX-License-Identifier: Apache-2.0
// #217 (D13): a pointer that is const itself and whose pointed object is only read. The rule
// suggests const for the pointed object; the check tries the plan's candidate, which keeps the
// pointer's own const, and the suggestion displays that candidate.

// strict-expectation-details: true

// at line 18, column 0
// [ !Info! ] ConstParameterNotModified.PointerConstOnly: parameter 'p' in function 'read_only' is declared 'int * const p' but the pointed object is never modified
// ↳ consider 'const int *const p' for API const-correctness
// ↳ current type: int * const p
// ↳ suggested type: const int *const p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// analyzer-arg: --experimental-const-suggestion-check

int read_only(int* const p)
{
    return *p;
}
