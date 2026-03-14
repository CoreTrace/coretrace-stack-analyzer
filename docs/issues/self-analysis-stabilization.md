# Stabilize self-analysis: fix false positives, bitfield handling, and warning hygiene (LLVM/SMT)

## Description
After struct layout/padding updates, self-analysis started reporting extra false positives (mainly `uninitialized` and `duplicate-if`) and compilation warnings (`-Wreorder-init-list`).

The goal is to fix behavior in the analyzer itself (no hardcoded suppression hacks), while keeping generic logic and backend compatibility (including SMT backends).

## What was done
1. Refactored local control/data-flow paths to remove `potential read of uninitialized local variable` false positives without changing business semantics.
2. Improved `UninitializedVarAnalysis` to better handle constructor/bitfield read-modify-write (`this`) patterns and avoid non-actionable caller-side reports.
3. Refined `DuplicateIfCondition` branch reasoning (reachability/equivalent conditions) to reduce unreachable `else-if` false positives.
4. Fixed `-Wreorder-init-list` warnings in SMT stack components (`SmtRefinement`, `SmtEncoding`, `Z3Backend`) by matching designated-init order to declaration order.
5. Added false-positive repro tests and bitfield coverage tests to lock regressions.
6. Updated related project surfaces (docs/config/build/analyzer/app/cli) impacted by the fixes.

## Architecture rationale
- Applied fixes in analysis engines instead of masking diagnostics.
- Kept rules generic and reusable (no single-case hardcoded patch).
- Preserved SMT backend compatibility, including Z3 when selected.

## Acceptance criteria
- Target FP repros are covered by dedicated tests.
- `-Wreorder-init-list` warnings do not reappear on corrected units.
- No intended semantic regression on valid diagnostics.

## Follow-up
- Track remaining cross-TU instability observed in full-run campaign (2 scenarios still inconsistent) in a separate issue.
