# Add BTP proof scripts for stack analyzer feature validation

## Description
Add standalone Python proof scripts that demonstrate selected
`stack_usage_analyzer` capabilities using existing repository fixtures.

The scripts are intended for human review, BTP validation, and lightweight
feature evidence. They should make the analyzer command, analyzed fixture,
feature reference, PASS/NONE status, and raw evidence visible in the terminal.

## What was implemented
1. Added `BTP-STACK-ANALYZER-F1.py` for stack usage analysis proof:
   - static and propagated stack size,
   - dynamic stack size through VLA and `alloca`,
   - unknown stack propagation,
   - infinite recursion,
   - stack pointer escape,
   - explicit IR and ABI mode coverage.
2. Added `BTP-STACK-ANALYZER-F2.py` for advanced stack diagnostics proof:
   - `memcpy` overflow,
   - `memset` overflow,
   - deep aliasing,
   - basic const-correctness diagnostics.
3. Added `BTP-STACK-ANALYZER-F5.py` for output-format proof:
   - three fixtures,
   - `--format=human`,
   - `--format=json`,
   - `--format=sarif`,
   - parseable JSON/SARIF checks,
   - readable CLI checks,
   - cross-format diagnostic count consistency,
   - raw output printed for every format.
4. Added `BTP-STACK-ANALYZER-F8.py` for Itanium ABI mangle/demangle proof:
   - JSON raw symbol extraction,
   - human output with `--demangle`,
   - visible mangled and demangled symbols,
   - parameterized C++ signatures,
   - repeated-run stability.
5. Added `BTP-STACK-ANALYZER-F10.py` for static stack-buffer overflow proof:
   - existing stack-buffer fixture,
   - `StackBufferOverflow` diagnostic,
   - function, variable, severity, rule id, and diagnostic text.
6. Added `BTP-STACK-ANALYER.py` as an earlier combined proof helper for:
   - demangle proof,
   - static stack-buffer overflow proof.
7. Updated `test/test.cpp` with C++ functions covering multiple parameter shapes
   so F8 can prove demangled signatures with parameters and overloads.
8. Standardized proof output:
   - feature prefix derived from the script name, e.g. `[F1]`, `[F5]`, `[F8]`,
   - purple fixture names,
   - green `PASS`,
   - red `NONE`,
   - `--------` separators between fixture reports.

## Architecture rationale
- Keep each BTP feature in its own script so each competency maps to one
  executable proof artifact.
- Reuse existing repository fixtures instead of generating temporary source
  files, preserving alignment with the analyzer test corpus.
- Parse structured JSON/SARIF output with Python `json` instead of relying only
  on string matching.
- Keep display logic local to each standalone script to avoid import/path
  fragility when scripts are run directly from the repository root.
- Derive feature references from filenames instead of hardcoding `[F1]`,
  `[F2]`, etc., so future `BTP-STACK-ANALYZER-F*.py` scripts can reuse the same
  pattern.

## Validation commands
Run the proof scripts:

```bash
python3 -B BTP-STACK-ANALYZER-F1.py
python3 -B BTP-STACK-ANALYZER-F2.py
python3 -B BTP-STACK-ANALYZER-F5.py
python3 -B BTP-STACK-ANALYZER-F8.py
python3 -B BTP-STACK-ANALYZER-F10.py
```

Negative evidence checks:

```bash
python3 -B BTP-STACK-ANALYZER-F8.py test/security/buffer-overflow/01_buffer_overflow.c
python3 -B BTP-STACK-ANALYZER-F10.py --fixture test/no-error/basic-main.c
```

Full test suite previously validated:

```bash
python3 -B run_test.py --jobs 4
```

Observed result:

```text
Passed 1671/1671 tests
```

## Acceptance criteria
- F1 prints IR and ABI mode evidence and passes all stack-usage checks.
- F2 proves memory intrinsic overflow, deep aliasing, and const-correctness
  diagnostics.
- F5 proves human, JSON, and SARIF output over three files and prints the raw
  output for every mode.
- F8 proves Itanium ABI mangle/demangle with visible symbols and parameterized
  demangled signatures.
- F10 proves static stack-buffer overflow diagnostics on a stack-allocated
  buffer.
- Every proof script prints its feature reference before logs.
- `PASS` is green, `NONE` is red, fixture names are purple, and fixture sections
  are separated by `--------`.

## Follow-up
- Decide whether SARIF should remain standard-only or include a custom
  `properties.diagnosticsSummary` extension. Current F5 computes the SARIF
  summary from `runs[0].results` instead of expecting a non-standard
  `diagnosticsSummary` field.
- Consider extracting the repeated color/reference helpers into a shared module
  only if these scripts become maintained as a long-term test harness.
