# Pipeline Subscriber Classification

This document records the Phase-1 classification of analysis steps for the
single-pass subscriber architecture.

## Classification Rules

- `subscriber-compatible`: primarily instruction-pattern driven checks that can
  consume event streams (`alloca`, `load`, `store`, `call`, `invoke`,
  `memintrinsic`) without requiring a fixpoint over inter-procedural summaries.
- `independent`: analyses that rely on iterative dataflow/fixpoint solving,
  cross-function summary propagation, or specialized graph/state convergence.
- `utility`: orchestration/preparation steps (not diagnostics analyses).

The split mirrors LLVM pass-planning guidance (explicit dependencies and
analysis invalidation boundaries) and keeps behavior deterministic during
rollout.

## Current Mapping

| Pipeline step | Class | Reason |
|---|---|---|
| Function attrs pass | utility | Canonicalization pass, no diagnostic rule. |
| Prepare module | utility | Context/call graph/recursion derivation. |
| Collect IR facts | utility | Shared fact collection pass. |
| Build results | utility | Output scaffolding. |
| Emit summary diagnostics | utility | Aggregation-only emit phase. |
| Compute alloca threshold | utility | Config-derived threshold materialization. |
| Stack buffer overflows | subscriber-compatible | Event-driven from stack writes and allocation patterns. |
| Dynamic allocas | subscriber-compatible | Direct `alloca`-shape detection. |
| Alloca usage | independent | Depends on recursion/global stack metadata coupling. |
| Mem intrinsic overflows | subscriber-compatible | Mem intrinsic rule matching over instruction stream. |
| Integer overflows | independent | Range/dataflow-heavy with conservative propagation. |
| Size-minus-k writes | independent | Wrapper-summary propagation to fixpoint. |
| Multiple stores | subscriber-compatible | Store-pattern correlation over stack slots. |
| Duplicate if conditions | subscriber-compatible | Intra-function condition canonicalization and matching. |
| Uninitialized local reads | independent | Inter-procedural summary + CFG/dataflow reasoning. |
| Global reads before writes | independent | Global state flow + summary-based tracking. |
| Invalid base reconstructions | subscriber-compatible | Local reconstruction chains, no inter-proc fixpoint loop. |
| Stack pointer escapes | independent | Inter-procedural fixed-point with conservative caps. |
| Const params | subscriber-compatible | Use-def traversal around argument write/read patterns. |
| Null pointer dereferences | independent | Branch-sensitive path/state reasoning. |
| Out-of-bounds reads | independent | Bounds/range refinement over dataflow paths. |
| Command injection | subscriber-compatible | Call-site taint/pattern checks from instruction events. |
| TOCTOU | subscriber-compatible | API call ordering checks in function-level streams. |
| Type confusion | subscriber-compatible | Type/layout pattern checks over pointer casts/GEPs. |
| Resource lifetime | independent | Summary propagation across calls and class lifecycle states. |

## Migration Notes

- Fully migrated to subscriber-shared gating:
  - `Stack buffer overflows`
  - `Resource lifetime` (safe call-site absence short-circuit)
- `Uninitialized local reads` remains `independent` after validation showed
  that a simple `load`-based skip is unsound.
- Candidate next migrations (subscriber-compatible class):
  - `Dynamic allocas`
  - `Mem intrinsic overflows`
  - `Multiple stores`
  - `Duplicate if conditions`
  - `Command injection`
  - `TOCTOU`
