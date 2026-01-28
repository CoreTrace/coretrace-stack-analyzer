# Const-correctness diagnostic for pointer/reference parameters

## Summary
We added a static diagnostic that flags function parameters (pointers and references) that are never used to modify the pointed/referred object and can therefore be made `const`. The diagnostic is emitted as `ConstParameterNotModified` with the standard human-readable severity prefix and sub-labels:
- Info -> `[!]`
- Warning -> `[!!]`
- Error -> `[!!!]`

The rule focuses on API const-correctness and is conservative to avoid false positives.

## What was added
- New diagnostic rule id: `ConstParameterNotModified` in `include/StackUsageAnalyzer.hpp`.
- IR-based analysis in `src/StackUsageAnalyzer.cpp` to detect read-only pointee/referent usage and emit a suggestion.
- Debug-info parsing to reconstruct parameter names and source-level types, with qualifier preservation.
- Demangling in the diagnostic message (function name shown in human-readable form when possible).
- Test cases in `test/pointer_reference-const_correctness/` (includes rvalue reference case).
- Test runner updated to include `.cpp` files (`run_test.py`).

## Detection logic (current behavior)
### Scope
- Only function parameters (not locals).
- Pointer and reference types only.
- Skips double pointers (e.g. `T**`, `T*&`), `void*`, and function pointers (conservative).
- Skips parameters already `const` at the pointee/referent level.

### What counts as modifying
A parameter is considered *modifiable* (no suggestion) if any of these are detected:
- Direct stores through the pointer (e.g., `*p = ...`, `p[i] = ...`).
- Atomic RMW / atomic cmpxchg on the derived pointer.
- Passed as an argument to a callee that may write through it (conservative rules below).
- Pointer escapes to unknown memory (e.g., pointer stored in a non-local location).

### Interprocedural handling (conservative)
When the parameter is passed to a call:
- If callee is known and param is `const T*` / `const T&` -> read-only.
- Otherwise -> assume it may write.
- Variadic / indirect / unknown calls -> assume it may write.
- LLVM function attributes `readnone`/`readonly` are honored.
- Mem intrinsics are handled: `memcpy/memmove/memset` treat destination as write.

### O0 / debug-friendly pointer flow
Handles the common pattern where an argument is spilled to an `alloca` and then reloaded:
- If the argument is stored into a local alloca and later loaded, the analysis follows the load and can still detect writes through the parameter (prevents false positives like `*p = 0`).

## Type reconstruction
- Parameter types and names are extracted from debug info (`DILocalVariable`/`DISubroutineType`).
- Qualifiers (`const`, `volatile`, `restrict`) are preserved.
- Suggested type adds `const` to the *pointee/referent* (not just the pointer variable).
- Special case: `T * const param` produces a message explaining pointer-const doesn’t protect the pointee and suggests `const T *`.

## Diagnostic format (human readable)
Example:
```
[!]ConstParameterNotModified.Pointer: parameter 'param3' in function 'myfunc' is never used to modify the pointed object
       current type: int32_t *param3
       suggested type: const int32_t *param3
```

When the parameter is pointer-const only:
```
[!]ConstParameterNotModified.PointerConstOnly: parameter 'param4' in function 'myfunc' is declared 'int32_t * const param4' but the pointed object is never modified
       consider 'const int32_t *param4' for API const-correctness
       current type: int32_t * const param4
       suggested type: const int32_t *param4
```

Rvalue reference example:
```
[!]ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'value' in function 'read_once(int&&)' is an rvalue reference and is never used to modify the referred object
       consider passing by value (int value) or const reference (const int &value)
       current type: int &&value
```

## Files changed
- `include/StackUsageAnalyzer.hpp`
  - Added `ConstParameterNotModified` to `DescriptiveErrorCode`.
- `src/StackUsageAnalyzer.cpp`
  - New const-parameter analysis and debug type formatting helpers.
  - Conservative call handling and write detection.
  - Demangling in diagnostic messages.
- `run_test.py`
  - Now includes `.cpp` files for test expectations.
- `test/pointer_reference-const_correctness/`
  - `readonly-pointer.c`
  - `readonly-reference.cpp`
  - `const-readonly.c`
  - `const-mixed.c`
  - `advanced-cases.c`
  - `advanced-cases.cpp`

## Notes / Known conservative behavior
- `volatile` pointers currently still get a const suggestion (`const volatile T*`) if read-only.
- Variadic calls are treated as mutating (conservative).
- Double pointers are skipped to avoid invalid suggestions.

## Possible follow-ups (optional)
- Skip suggestions on `volatile` pointees (if you want to preserve MMIO patterns).
- Whitelist read-only variadic functions (`printf`, `puts`, etc.) to reduce false negatives.
- Sub-labels added: `ConstParameterNotModified.Pointer`, `ConstParameterNotModified.Reference`, `ConstParameterNotModified.PointerConstOnly`, `ConstParameterNotModified.ReferenceRvaluePreferValue`.
