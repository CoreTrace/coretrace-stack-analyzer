# Add LLVM Function/Call-Site Attribute Introspection for Failure Semantics

## Description
We need a first-class way to inspect LLVM IR attributes attached to functions and call sites in order to support failure-oriented checks (e.g., may-unwind, allocation behavior, return contracts).

Current analysis can read IR and diagnostics, but there is no explicit user-facing feature to dump and audit attribute-level semantics per symbol/call site.

## Goal
Add a dedicated analyzer feature to export attribute information per function and per `call`/`invoke` so users can understand which failure channels are inferable from IR and which require external models.

## Why this matters
- Failure checks depend on reliable low-level signals (`nounwind`, EH shape, allocator attributes, return pointer contracts).
- `[[nodiscard]]` and similar frontend-only contracts are not consistently represented in LLVM IR; users need clear visibility into what is and is not available in IR.
- A deterministic dump reduces ambiguity when designing generic rules (no hardcoded API assumptions).

## Proposed architecture
1. Add an `AttributeIntrospection` component under analyzer modules (read-only utility service).
2. Integrate it in pipeline orchestration as an optional stage (enabled by CLI/config flag).
3. Collect:
   - function attributes (`Function::getAttributes().getFnAttrs()`)
   - return/param attributes relevant to failure semantics
   - call-site attributes from `llvm::CallBase` (`call` and `invoke`)
   - EH structural hints (`invoke` unwind edge, personality presence, landing pad family)
4. Emit output in deterministic machine-readable form (JSON first; optional human section).
5. Keep behavior generic:
   - no hardcoded list of STL or libc symbol names
   - unknown attributes preserved as raw key/value when possible

## CLI/config surface (proposal)
- `--dump-attrs` enable attribute introspection output
- `--dump-attrs-out=<path>` write JSON artifact (default: stderr in human mode)
- optional filters (reuse existing filters):
  - `--only-function`
  - `--only-file` / `--only-dir`

## Data contract (minimal)
Per function:
- `functionName`, `demangledName?`, `sourcePath?`
- `fnAttributes[]`
- `returnAttributes[]`
- `paramAttributes[{index, attrs[]}]`
- `hasPersonality`, `personalityName?`

Per call site:
- `caller`, `location?`
- `kind` (`call` | `invoke`)
- `directCallee?`
- `callSiteAttributes[]`
- `hasUnwindEdge` (for invoke)

## Non-goals
- No semantic verdict yet ("unchecked failure" rule is a separate issue).
- No hardcoded mapping from symbol name to "must check" policy in this issue.
- No test file edits without explicit authorization.

## Acceptance criteria
- New option(s) produce deterministic output for a module with mixed `call`/`invoke`.
- Output includes function-level and call-site-level attributes.
- Unknown/string attributes are preserved (not silently dropped).
- Works for LLVM IR inputs and source-compiled inputs.
- Documentation explains IR visibility limits (e.g., frontend-only attributes).

## Risks / tradeoffs
- Attribute sets vary by toolchain/version/optimization level; output must avoid over-promising semantic certainty.
- Excessive verbosity on large modules; output should be filterable and optionally file-based.

## Follow-up
- Build `UncheckedFailure` diagnostics using this introspection plus external API models.
