# Replace the hand-written slot reasoning with MemorySSA-backed reaching definitions

## Description
`FunctionFacts` now owns the upstream LLVM analyses used to bound values
(`AssumptionCache`, `DominatorTree`, `TargetLibraryInfo`, `LazyValueInfo`,
`ObjectSizeOffsetVisitor`, and `computeConstantRange`). Wiring them in exposed a
structural limit that no amount of additional register-level analysis can lift.

`InputPipeline` strips any caller-supplied `-O` flag and appends `-O0`
(`src/analysis/InputPipeline.cpp:122` and `:176`). That choice is deliberate: it
preserves the source-to-IR mapping the diagnostics depend on. Its consequence is
that every local lives in an `alloca` and every value an analysis wants to reason
about is a `load`. LazyValueInfo, KnownBits, ScalarEvolution and `llvm.assume`
all reason over SSA registers, so they see nothing through that indirection.

The current workaround, `publishSingleStoreSlots` in `src/analysis/IntRanges.cpp`,
is a hand-written partial `mem2reg`: it recognises an integer `alloca` that is
written exactly once and never has its address taken, and mirrors the stored
value's range onto the slot and its loads. It works, and it is what makes the
range facts reach any consumer at all, but it is deliberately narrow and it is the
fourth copy of the same idiom in the codebase.

MemorySSA is the one analysis in this family that is unaffected by `-O0`, because
it models memory rather than registers. It answers "which store reaches this
load" directly, which is precisely the question the four hand-written copies
approximate.

## Current state
Four independent implementations of single-store slot resolution:

| Location | Flavour |
|---|---|
| `src/analysis/DuplicateIfCondition.cpp:350` | pointer slots, via `resolvePointerSource` |
| `src/analysis/TypeConfusionAnalysis.cpp:89` | pointer slots, `peelPointerFromSingleStoreSlot` |
| `src/analysis/OOBReadAnalysis.cpp` | pointer slots, `peelPointerFromSingleStoreSlot` |
| `src/analysis/IntRanges.cpp` | integer slots, `singleStoredInteger` |

All four fail on the same input shape: a slot written on more than one path.

## Proposed change
1. Add `AAResults` to `FunctionFacts`. `BasicAA` requires `TargetLibraryInfo`, a
   `DominatorTree` and an `AssumptionCache`, all of which `FunctionFacts` already
   owns, so this is a construction-order change rather than new infrastructure.
2. Add `MemorySSA` on top of it, built lazily like the other members.
3. Expose a single reaching-definition query on `FunctionFacts` and reimplement
   `publishSingleStoreSlots` in terms of it, generalising from "exactly one store"
   to "join of the reaching definitions".
4. Migrate the three pointer-flavoured copies onto the same query, in separate
   commits, so each migration can be reverted independently.

## Expected outcome
- Slots written on more than one path become analysable. This is currently pinned
  as an explicit absence of information by `testIntRangeFacts` in
  `test/unit/analyzer_module_unit_tests.cpp`; that assertion becomes a positive
  one, asserting the join of both stored ranges.
- `llvm.assume` becomes effective. The limitation recorded in
  `test/security/oob-read/heap-index-builtin-assume-limitation.c` is a reaching
  definition problem: the assumption constrains the SSA load that
  `__builtin_assume` was applied to, while the indexed access reads the slot
  through a second, distinct load. Tied to a common memory definition, the fact
  transfers. That fixture must then be inverted, as its header comment states.
- `AAResults` becomes available to `TOCTOUAnalysis` and
  `GlobalReadBeforeWriteAnalysis`, which currently compare access roots by
  pointer identity on the result of `getUnderlyingObject`.
- Four copies of slot resolution collapse into one owner.

## Non-goals
- Raising the optimisation level of the clang invocation. Diagnostics that vary
  with `-O` are not testable against a single expectation corpus, and anything
  above `-O0` degrades the source mapping the reports are built on. See the
  discussion under "Alternatives considered".
- Reintroducing `ScalarEvolution` and `LoopInfo`. They were removed as measurably
  inert at `-O0`; they should be reconsidered only once this issue lands, since a
  loop counter lives in a multiply-assigned slot that only reaching-definition
  information can see through.

## Alternatives considered
**Promote locals to registers on a cloned module.** Running `mem2reg`/SROA on a
copy of the module used only for fact computation would unlock every register-level
analysis at once. It was rejected because mapping values back to the original
module is the hard part: after promotion an `alloca` disappears and its loads
become SSA values, so there is no pointer-stable correspondence, and reconstructing
one from debug metadata is fragile. MemorySSA delivers the specific capability that
is missing without needing a second module.

**Gate the analyses on the optimisation level.** Not actionable on the clang-driven
path, where `-O0` is forced and user `-O` flags are stripped. For `.ll`/`.bc` inputs
parsed directly by `parseIRFile` (`src/analysis/InputPipeline.cpp:1393`) the input
may already be optimised, but there is no optimisation level to read there; the
signal would have to be a measured property of the IR, not a flag.

## Acceptance criteria
- `FunctionFacts` exposes one reaching-definition query, built lazily, and remains
  the only owner of the analyses it wires together.
- `singleStoredInteger` is gone, replaced by that query.
- `testIntRangeFacts` asserts a joined range for the multiply-assigned slot in
  `test/unit/int_range_facts_input.c`, instead of asserting absence.
- `heap-index-builtin-assume-limitation.c` is inverted to a `// not contains:`
  expectation and renamed accordingly.
- The mutation checks in `docs/architecture/llvm-analysis-integration.md` still
  hold: breaking each guard turns at least one test red.
- No regression against the current baseline.

```zsh
cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON && cmake --build build
python3 -B run_test.py --jobs 8
```

Current result to preserve or improve:

```text
Passed 1711/1711 tests
```

## Follow-up
- Once reaching definitions are available, re-measure `ScalarEvolution` on the
  same corpus before deciding whether to restore it.
- Decide whether the pointer-flavoured migrations belong in this issue or in a
  dedicated consolidation issue, based on how much diagnostic churn the first one
  produces.
