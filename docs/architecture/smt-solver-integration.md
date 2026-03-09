# SMT Solver Integration Architecture

## Context

This document defines a generic architecture to integrate SMT solving (Z3-style)
into `coretrace-stack-analyzer` while keeping:

- backend interchangeability (Z3, cvc5, etc.),
- coupled execution modes (single, portfolio, cross-check),
- controlled performance cost,
- predictable diagnostics behavior.

This design targets C++20 and avoids hard-coded backend logic in analysis passes.

## Goals

1. Reduce false positives on ambiguous findings.
2. Keep current fast heuristic analysis as first-pass filter.
3. Allow changing solver backend without refactoring analysis code.
4. Allow running multiple backends together for stronger confidence.
5. Keep failure modes safe (`UNKNOWN` must not hide real issues).

## Non-goals

1. Replacing all existing analyses with full symbolic execution.
2. Solving every finding with SMT by default.
3. Depending on solver-specific AST types inside domain logic.

## Design Principles

1. **Layered architecture**: analysis passes never call solver APIs directly.
2. **Strategy pattern**: solver execution policy is selectable at runtime.
3. **Adapter pattern**: each SMT backend is an adapter behind a shared interface.
4. **Fail-safe defaults**: timeout/error/unknown keep diagnostics conservative.
5. **Bit-precise semantics**: encoding follows LLVM integer widths and signedness.

## High-level Architecture

```text
LLVM IR
  -> Analysis Pass (existing heuristic pass)
  -> Constraint Builder (LLVM -> ConstraintIR)
  -> Solver Orchestrator (execution mode policy)
      -> ISmtBackend (Z3Backend / Cvc5Backend / ...)
  -> Diagnostic Refiner (keep/suppress/downgrade)
```

## Proposed Module Split

```text
include/analysis/smt/
  ConstraintIR.hpp
  SolverTypes.hpp
  ISmtBackend.hpp
  ISolverStrategy.hpp
  SolverOrchestrator.hpp
  DiagnosticRefiner.hpp

src/analysis/smt/
  ConstraintIR.cpp
  SmtEncoding.cpp
  SolverOrchestrator.cpp
  strategies/SingleSolverStrategy.cpp
  strategies/PortfolioSolverStrategy.cpp
  strategies/CrossCheckSolverStrategy.cpp
  backends/Z3Backend.cpp
  backends/Cvc5Backend.cpp
```

## Core Contracts

### Solver status

```cpp
enum class SmtStatus { Sat, Unsat, Unknown, Timeout, Error };
```

### Query and answer

```cpp
struct SmtQuery
{
    ConstraintIr ir;
    std::string ruleId;
    std::uint32_t timeoutMs = 50;
    std::uint64_t budgetNodes = 10000;
};

struct SmtAnswer
{
    SmtStatus status = SmtStatus::Unknown;
    std::string backendName;
    std::optional<Model> model;
    std::optional<std::string> reason;
};
```

### Backend interface

```cpp
class ISmtBackend
{
public:
    virtual ~ISmtBackend() = default;
    virtual std::string name() const = 0;
    virtual SmtAnswer solve(const SmtQuery& query) = 0;
};
```

### Strategy interface

```cpp
class ISolverStrategy
{
public:
    virtual ~ISolverStrategy() = default;
    virtual std::vector<SmtAnswer> run(const SmtQuery& query) = 0;
};
```

## ConstraintIR (backend-agnostic)

`ConstraintIR` is an internal logical IR. It isolates analysis semantics from
solver syntax.

### Required expression set (v1)

1. Bit-vector constants and variables.
2. Arithmetic ops: add/sub/mul, shifts.
3. Comparisons: signed and unsigned variants.
4. Boolean ops: and/or/not/implies.
5. Cast-like ops: zext/sext/trunc.
6. Optional support for arrays/memory in later iteration.

## Solver Orchestration Modes

### 1) Single mode

- One configured backend.
- Lowest cost.

### 2) Portfolio mode (coupled)

- Run multiple backends in parallel.
- Configurable decision policy:
  - `first_unsat`,
  - `first_sat`,
  - `quorum`.

### 3) Cross-check mode (coupled)

- Primary backend solves first.
- Secondary backend only runs on:
  - `Unknown`,
  - timeout,
  - selected high-risk rules.

### 4) Dual-consensus mode (strict FP reduction)

- Suppress finding only if all selected backends conclude `Unsat`
  for bug-feasibility query.

## Diagnostic Refinement Policy

Given bug-feasibility query:

1. `Sat`: keep diagnostic (bug path feasible).
2. `Unsat`: suppress or downgrade according to rule policy.
3. `Unknown` / `Timeout` / `Error`: keep diagnostic and mark as inconclusive.

This avoids unsound suppression.

## Integration in Existing Pipeline

Integration point: after heuristic detection, before final emission.

Suggested first adopters:

1. `IntegerOverflowAnalysis`
2. `SizeMinusKWrites`
3. `StackBufferAnalysis` ambiguous index-range cases

Rationale:

- These passes already expose constraint-like logic and are sensitive to FP.
- They can benefit early from feasibility checks with bounded cost.

## Configuration Surface (CLI)

Suggested options:

```text
--smt=off|on
--smt-backend=z3|cvc5
--smt-mode=single|portfolio|cross-check|dual-consensus
--smt-secondary-backend=<name>
--smt-timeout-ms=<N>
--smt-budget-nodes=<N>
--smt-rules=<comma-separated-rule-ids>
```

Default profile recommendation:

- `--smt=on` only for selected rule set.
- short timeout per query.
- max query size budget.

## Caching

Add process-local query cache:

- key: normalized `ConstraintIR` hash + solver mode/profile.
- value: `SmtStatus` (+ optional model fingerprint).

Benefits:

- avoid repeating equivalent queries,
- keep portfolio mode cost acceptable.

## Observability

Add counters and timing:

1. total SMT queries,
2. per-status counts,
3. per-backend latency percentiles,
4. suppression count by rule,
5. timeout/error rate.

This is required to track precision/performance tradeoffs.

## Validation Strategy

1. Keep all current regression tests unchanged.
2. Add SMT-focused fixtures in new files only.
3. For each onboarded rule:
   - one SAT expected case,
   - one UNSAT expected case (FP suppression),
   - one UNKNOWN/timeout fallback case.
4. Run parity checks for `--smt=off` vs `--smt=on` where expected.

## Rollout Plan

### Phase 1: Infrastructure

- Add interfaces, `ConstraintIR`, one backend adapter, single mode only.
- Keep feature behind `--smt=on`.

### Phase 2: First rule integration

- Integrate into integer-overflow related findings.
- Measure FP delta and runtime overhead.

### Phase 3: Coupled modes

- Add portfolio/cross-check strategy implementations.
- Add cache + telemetry.

### Phase 4: Extend to other rules

- Integrate selected stack-buffer and size-arg ambiguous diagnostics.
- Tune budgets per rule category.

## Current Implementation Status (March 2026)

Implemented in codebase:

1. Generic solver contracts (`ConstraintIR`, `ISmtBackend`, `ISolverStrategy`, orchestrator).
2. Runtime solver modes (`single`, `portfolio`, `cross-check`, `dual-consensus`).
3. CLI/config surface (`--smt=*`, backend/mode/timeout/budget/rules).
4. Recursion rule onboarding using a dedicated encoder (`LLVM range state -> ConstraintIR`).
5. Conservative fallback policy for recursion (`Unknown`/`Timeout`/`Error` never suppresses baseline diagnostics).
6. Optional Z3 backend integration with CMake auto-detection and safe fallback when unavailable.

Planned next:

1. Extend encoder coverage beyond interval-derived constraints for richer path conditions.
2. Add query cache and telemetry counters (query count/status/latency).
3. Onboard SMT to additional high-FP rules (integer overflow, size-minus-k, ambiguous stack buffer cases).

## Risk Register

1. **Runtime blowup** on large formulas:
   - mitigate with query size budget + timeout + staged solving.
2. **Unsound suppression due to encoding bug**:
   - mitigate with dual-consensus for suppressions and targeted tests.
3. **Backend drift** (different solver behavior):
   - mitigate with cross-check mode and backend-specific CI jobs.

## Why this approach

Compared to embedding Z3 calls directly in each pass, this architecture:

1. keeps domain logic independent from solver SDKs,
2. allows backend replacement without refactoring analyses,
3. enables coupled execution modes as first-class policies,
4. scales progressively with controlled technical risk.
