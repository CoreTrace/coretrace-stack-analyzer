# Contributing to coretrace-stack-analyzer

Thanks for contributing.

This document defines the expected workflow for code, tests, and pull requests.

## Development Setup

Prerequisites:
- CMake `>= 3.16`
- LLVM/Clang `>= 19` (20 recommended)
- A C++20 compiler

Build:

```bash
./build.sh --type Release
```

If LLVM/Clang are not auto-detected:

```bash
LLVM_DIR=/path/to/llvm/lib/cmake/llvm \
Clang_DIR=/path/to/llvm/lib/cmake/clang \
./build.sh --type Release
```

## Local Validation Before Opening a PR

Run formatting check:

```bash
./scripts/format-check.sh
```

Run regression tests:

```bash
python3 run_test.py --analyzer ./build/stack_usage_analyzer
```

Optional module unit tests (recommended for architectural/internal changes):

```bash
cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON
cmake --build build
cd build && ctest -R analyzer_module_unit_tests
```

## Commit Convention (CI Enforced)

Commit subjects must follow Conventional Commits:

```text
type(scope): subject
```

Allowed `type` values:
- `feat`
- `fix`
- `chore`
- `docs`
- `refactor`
- `perf`
- `ci`
- `build`
- `style`
- `revert`
- `test`

Rules:
- Subject line max length: **84** characters.
- Use English for commit messages.
- Keep commits focused and atomic when possible.

## Pull Request Expectations

A PR should include:
- A clear problem statement and solution summary.
- Behavioral impact (what changed for users/CI/API).
- Validation evidence (commands run, test results).
- Documentation updates when behavior/options/contracts change.

If relevant, include:
- Example CLI invocation
- JSON/SARIF output impact
- Notes on profile/cross-TU performance impact

## Architecture Guardrails

When adding or refactoring features, preserve module boundaries:

- `src/app/AnalyzerApp.cpp`: application orchestration, strategy selection, input planning.
- `src/analyzer/*`: analysis pipeline coordination, preparation, location resolution, diagnostic emission.
- `src/analysis/*`: analysis logic and findings generation.
- `src/report/ReportSerialization.cpp`: output serialization (JSON/SARIF).
- `src/cli/ArgParser.cpp`: CLI argument parsing and validation.

Why:
- Keeps analysis logic decoupled from CLI/CI concerns.
- Improves testability with narrow module responsibilities.
- Reduces regression risk by centralizing reporting and orchestration behavior.

For architecture details, see:
- `docs/architecture/analyzer-modules.md`

## Adding a New Check (Short Version)

1. Implement analysis logic under `src/analysis/` (+ header under `include/analysis/`).
2. Integrate the pass into pipeline/module orchestration.
3. Emit diagnostics through `DiagnosticEmitter` (severity, rule ID, message, CWE/confidence if applicable).
4. Add regression tests under `test/`.
5. Update docs (`README`, wiki/docs pages) when new behavior or options are introduced.

## CI Notes

Current CI validates at least:
- Conventional commit format
- clang-format compliance
- build/tests/integration workflows

Before opening a PR, run local checks to reduce CI round-trips.
