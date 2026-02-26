# Analyzer Modules Architecture

This document describes the module split introduced around `StackUsageAnalyzer` to reduce coupling, improve testability, and keep `main.cpp` and the public API focused on orchestration.

## Goals

- Keep analysis orchestration separated from LLVM parsing and diagnostic formatting details.
- Make core services independently testable with focused unit tests.
- Preserve existing integration behavior while enabling smaller regression checks.

## Modules

### `src/analyzer/AnalysisPipeline.cpp`

Role:
- Entry point for module-level analysis execution.
- Coordinates preparation, analysis passes, and diagnostic emission.

Pattern:
- `Facade` over lower-level analysis services.

Why:
- A single coordinator makes control flow explicit while avoiding a very large `StackUsageAnalyzer.cpp`.

### `src/analyzer/ModulePreparationService.cpp`

Role:
- Builds `ModuleAnalysisContext`.
- Computes local stack sizes, filtered call graph, and recursion metadata.

Pattern:
- `Application Service` with a small `Builder-like` output (`PreparedModule`).

Why:
- Preparation logic is pure module state derivation and should be reusable without triggering diagnostic side effects.

### `src/analyzer/LocationResolver.cpp`

Role:
- Converts LLVM debug locations into normalized source coordinates.
- Resolves source location for allocas using debug intrinsics fallbacks.

Pattern:
- `Domain Service` (stateless policy logic).

Why:
- Location derivation has multiple LLVM-specific fallbacks; isolating it keeps diagnostics code simpler and easier to test.

### `src/analyzer/DiagnosticEmitter.cpp`

Role:
- Converts analysis findings into final diagnostics outputs.
- Central place for rule IDs, severities, and source location mapping.

Pattern:
- `Adapter` between analysis model objects and output/report models.

Why:
- Separates "what was found" from "how it is reported".

### `src/analysis/Reachability.cpp`

Role:
- Contains static reachability heuristics for stack access findings.

Pattern:
- `Policy` function isolated from pass execution.

Why:
- Reachability criteria can evolve independently and be regression-tested as a focused unit.

## Data Flow

1. Input pipeline loads/normalizes LLVM module.
2. `ModulePreparationService` creates `PreparedModule`.
3. Analysis passes compute findings.
4. `Reachability` filters/annotates specific findings.
5. `LocationResolver` provides precise locations.
6. `DiagnosticEmitter` produces diagnostics consumed by CLI/lib callers.

## Test Strategy

Fine-grained unit tests live in:
- `test/unit/analyzer_module_unit_tests.cpp`

Covered modules:
- `LocationResolver`
- `Reachability`
- `ModulePreparationService`

Execution:
- Built as `stack_usage_analyzer_unit_tests` (standalone project builds only).
- Wired into `run_test.py` via `check_analyzer_module_unit_tests()`.
- Also registered in CTest as `analyzer_module_unit_tests`.
