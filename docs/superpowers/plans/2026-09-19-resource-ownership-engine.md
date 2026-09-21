# Resource Ownership Engine — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the static acquire/release counting behind `ResourceLifetime.MissingRelease` with a flow-sensitive ownership engine, delivered complete on the spec's scope.

**Architecture:** Three components. `OwnershipFacts` (LLVM-free data) describes a function as blocks of typed events plus edges with edge events; `OwnershipEngine` (LLVM-free) propagates `(resource states, location contents)` to a fixpoint and computes transformer summaries; `ResourceFactCollector` (LLVM) turns an `llvm::Function` into facts by reusing the existing model-rule and storage recognition; `ResourceLifetimeAnalysis` replays stabilized states to emit `MissingRelease`.

**Tech Stack:** C++20, LLVM 20 IR API, existing `run_test.py` fixture runner and `stack_usage_analyzer_unit_tests`.

**Spec:** `docs/superpowers/specs/2026-09-19-resource-ownership-engine-design.md`

## Global Constraints

- No deliberately incorrect approximation: missing information yields *uncertain*, never a leak or a release (spec §1).
- `transfer(⊥) = ⊥` for every event (spec §3.1).
- Other `ResourceLifetime` rules and their counters are untouched (spec §1).
- Existing model files stay valid unchanged; `if_ret…` is an optional trailing qualifier (spec §4.1).
- Cross-TU cache schema becomes `cross-tu-resource-summary-v3` (spec §5).
- Every fixture change to `test/resource-lifetime/` other than the renaming of `acquire-returned-conditional-no-leak.c` is reported before being made (spec §9).
- Commits in the repository owner's name, conventional-commit subjects, `Refs #100`, no co-author.
- Build: `cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests -j 8`; unit tests: `./build/stack_usage_analyzer_unit_tests .`; fixtures: `python3 run_test.py --jobs 8 --no-cache`; format: `/opt/homebrew/opt/llvm@20/bin/clang-format -i <files>`.

---

## File structure

| File | Responsibility |
|---|---|
| `include/analysis/ownership/OwnershipDomain.hpp` | `OwnState`, `StateSet`, `Contents`, `AbstractState` with join/equality. LLVM-free. |
| `include/analysis/ownership/OwnershipFacts.hpp` | `Event`, `Block`, `Edge`, `Location`, `ParamTransformer`, `ExitTransformer`, `FunctionOwnershipSummary`, `OwnershipFacts`. LLVM-free. |
| `include/analysis/ownership/OwnershipEngine.hpp`, `src/analysis/ownership/OwnershipEngine.cpp` | `solve(facts, initial) → OwnershipResult`; `replay`; `computeSummary`. LLVM-free. |
| `include/analysis/ownership/ResourceFactCollector.hpp`, `src/analysis/ownership/ResourceFactCollector.cpp` | `collectOwnershipFacts(F, model, summaries, DL) → OwnershipFacts` + `LocationTable` mapping ids back to LLVM values. |
| `src/analysis/ResourceLifetimeAnalysis.cpp` | model grammar `if_ret…`; `MissingRelease` from engine; summary export/import with transformers. |
| `include/analysis/ResourceLifetimeAnalysis.hpp` | `ResourceSummaryFunction::normal/exceptional/incomplete`; `ResourceLifetimeIssueKind::AnalysisIncomplete`; `ResourceLifetimeIssue::exitLine`. |
| `src/app/AnalyzerApp.cpp` | cache schema v3, JSON of transformers. |
| `src/analyzer/DiagnosticEmitter.cpp` | messages for partial leak / reference loss / AnalysisIncomplete. |
| `test/unit/ownership_engine_unit_tests.cpp` (new executable target) | engine tests on synthetic CFGs. |
| `test/unit/analyzer_module_unit_tests.cpp` | collector tests on IR. |
| `test/resource-lifetime/*.c|.cpp` | end-to-end fixtures. |

---

### Task 1: Ownership domain

**Files:**
- Create: `include/analysis/ownership/OwnershipDomain.hpp`
- Create: `test/unit/ownership_engine_unit_tests.cpp`
- Modify: `CMakeLists.txt` (add `ownership_engine_unit_tests` target next to `stack_usage_analyzer_unit_tests`, registered in CTest and in `run_test.py::check_analyzer_module_unit_tests` alongside the existing binary)

**Interfaces — Produces:**

```cpp
namespace ctrace::stack::analysis::ownership {
enum class OwnState : std::uint8_t { NotOwned = 0, Owned = 1, Released = 2, Escaped = 3 };
struct StateSet {
    std::uint8_t bits = 0;
    static StateSet of(OwnState s);                 // singleton
    static StateSet none();                         // empty (⊥ component)
    [[nodiscard]] bool has(OwnState s) const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool isOnly(OwnState s) const;    // == {s}
    StateSet& operator|=(StateSet o); friend StateSet operator|(StateSet, StateSet);
    friend bool operator==(StateSet, StateSet);
};
using ResourceId = std::uint32_t;   // site s → new = 2*s, old = 2*s + 1
using LocationId = std::uint32_t;
struct Contents {                   // what a location may hold
    std::vector<ResourceId> resources;  // sorted, unique
    bool mayNull = false;
    bool mayUnknown = false;
    [[nodiscard]] bool isExactly(ResourceId r) const;   // {r}, no null/unknown
    [[nodiscard]] bool empty() const;
    void add(ResourceId r); void merge(const Contents& o);
    friend bool operator==(const Contents&, const Contents&);
};
struct AbstractState {
    bool reached = false;                 // false ⇔ ⊥
    std::vector<StateSet> resources;      // by ResourceId
    std::vector<bool> uncertain;          // sticky, by ResourceId
    std::vector<Contents> locations;      // by LocationId
    static AbstractState bottom(std::size_t resourceCount, std::size_t locationCount);
    static AbstractState entry(std::size_t resourceCount, std::size_t locationCount); // reached, all NotOwned, all empty
    void join(const AbstractState& o);    // ⊥ ⊔ x = x
    friend bool operator==(const AbstractState&, const AbstractState&);
};
}
```

- [ ] **Step 1: Write failing tests** in `test/unit/ownership_engine_unit_tests.cpp` (same `TestReport` pattern as `analyzer_module_unit_tests.cpp`, `main(argc, argv)` ignoring argv):

```cpp
bool testDomain(TestReport& r) {
    using namespace ctrace::stack::analysis::ownership;
    StateSet s = StateSet::of(OwnState::Owned);
    r.expect(s.isOnly(OwnState::Owned), "Domain: singleton isOnly");
    s |= StateSet::of(OwnState::Released);
    r.expect(s.has(OwnState::Owned) && s.has(OwnState::Released) && !s.isOnly(OwnState::Owned), "Domain: union");
    AbstractState b = AbstractState::bottom(2, 1);
    AbstractState e = AbstractState::entry(2, 1);
    r.expect(!b.reached && e.reached && e.resources[0].isOnly(OwnState::NotOwned), "Domain: bottom vs entry");
    AbstractState j = b; j.join(e);
    r.expect(j == e, "Domain: bottom is the join identity");
    Contents c; c.add(3); r.expect(c.isExactly(3), "Domain: exact contents");
    c.mayNull = true; r.expect(!c.isExactly(3), "Domain: null spoils exactness");
    return r.failures == 0;
}
```

- [ ] **Step 2: Add the CMake target** (copy the `stack_usage_analyzer_unit_tests` block, sources `test/unit/ownership_engine_unit_tests.cpp`, link `stack_usage_analyzer_lib`), configure with `-DBUILD_ANALYZER_UNIT_TESTS=ON`, build → expected: compile error, header missing.
- [ ] **Step 3: Write `OwnershipDomain.hpp`** implementing the interface above (bit ops on `bits`; `Contents::add` keeps the vector sorted-unique via `std::lower_bound`; `join` ORs resources, ORs uncertain, merges contents; `reached = reached || o.reached`).
- [ ] **Step 4: Build and run** `./build/ownership_engine_unit_tests` → all `Domain:` PASS.
- [ ] **Step 5: Commit** `test(ownership): domain lattice` then `feat(ownership): domain lattice (Refs #100)` — test commit first.

---

### Task 2: Facts and engine core (blocks, events, edges, fixpoint, exits)

**Files:**
- Create: `include/analysis/ownership/OwnershipFacts.hpp`, `include/analysis/ownership/OwnershipEngine.hpp`, `src/analysis/ownership/OwnershipEngine.cpp`
- Modify: `CMakeLists.txt` (add `src/analysis/ownership/OwnershipEngine.cpp` to `STACK_ANALYZER_SOURCES`), `test/unit/ownership_engine_unit_tests.cpp`

**Interfaces — Produces:**

```cpp
namespace ctrace::stack::analysis::ownership {
enum class Certainty : std::uint8_t { Guaranteed, Conditional, Unknown };
enum class LocationKind : std::uint8_t { Local, ArgPointee, Return, NonLocal }; // NonLocal = global/this-field/argument value
struct Location { LocationKind kind = LocationKind::Local; unsigned argIndex = 0; bool strongUpdatable = true; };
using ParamTransformer = std::array<StateSet, 4>;    // image of each singleton, index = OwnState
struct ExitTransformer { std::map<unsigned, ParamTransformer> params; Certainty returns = Certainty::Unknown; std::map<unsigned, Certainty> outArgs; bool present = false; };
struct FunctionOwnershipSummary { ExitTransformer normal; ExitTransformer exceptional; bool incomplete = false; };
struct CallEffect {                        // one modelled or summarised callee
    std::vector<std::pair<LocationId, ParamTransformer>> params;  // location holding the passed resource
    std::optional<LocationId> retDest; Certainty retCertainty = Certainty::Unknown;   // fresh resource into retDest
    std::vector<std::pair<LocationId, Certainty>> outArgs;        // fresh resource into *arg location
    std::uint32_t site = 0;                                       // acquisition site id for fresh resources
};
struct Event {
    enum class Kind : std::uint8_t { Acquire, Release, Copy, Overwrite, Return, AddressEscape, UnknownCall, Call, Exit };
    Kind kind;
    std::uint32_t site = 0;               // Acquire
    LocationId dst = 0, src = 0;          // Acquire(dst), Release(src), Copy(dst,src), Overwrite(dst), Return(src), AddressEscape(dst)
    bool strong = true;                   // Copy/Overwrite/Acquire strong update
    bool unknownValue = false;            // Overwrite: {Unknown} instead of {Null}
    Certainty certainty = Certainty::Guaranteed;
    std::vector<LocationId> args;         // UnknownCall
    CallEffect call;                      // Call
    bool exceptional = false;             // Exit / Call (apply exceptional transformer)
    std::uint32_t instructionIndex = 0;   // for diagnostics/replay
};
struct Block { std::vector<Event> events; };
struct Edge { std::uint32_t from = 0, to = 0; std::vector<Event> events; };   // edge events: conditional effects, invoke normal/unwind
struct OwnershipFacts {
    std::vector<Block> blocks;            // entry = block 0, RPO order
    std::vector<Edge> edges;
    std::vector<Location> locations;
    std::uint32_t siteCount = 0;          // resources = 2 * siteCount
    std::vector<std::pair<unsigned, ResourceId>> paramResources;  // summary mode: param i is initially in location with resource id
};
struct ExitRecord { std::uint32_t block, eventIndex; bool exceptional; AbstractState state; };
struct OwnershipResult { bool incomplete = false; std::vector<AbstractState> in, out; std::vector<ExitRecord> exits; };
OwnershipResult solve(const OwnershipFacts& facts, const AbstractState& entry, unsigned iterationLimit = 0);
// Replays block events from result.in[block]; callback(eventIndex, before, after).
void replay(const OwnershipFacts& facts, const OwnershipResult& result, std::uint32_t block,
            const std::function<void(std::uint32_t, const AbstractState&, const AbstractState&)>& cb);
// Applies one event; the single source of truth for transfer semantics (used by solve and replay).
void applyEvent(const OwnershipFacts& facts, const Event& e, AbstractState& s);
}
```

Transfer semantics (spec §3.2, §4, §6): `applyEvent` returns immediately if `!s.reached`. `Acquire`: `rn = 2*site, ro = rn+1`; `s.resources[ro] |= s.resources[rn]`; every location containing `rn` is retargeted to `ro`; `s.resources[rn] = {Owned}`; `dst` contents `= {rn}` if strong else `add(rn)`; if `certainty == Unknown` set `uncertain[rn]`. `Release`: for each `r` in `contents(src)`: strong (`isExactly(r)` and `Guaranteed`) → `= {Released}` else `|= {Released}`; if `contents(src).mayUnknown` nothing else. `Copy`: dst contents `=`/`merge` src contents; if `locations[dst].kind == NonLocal` or `ArgPointee`: each `r` in src → `|= {Escaped}` (strong if `isExactly`). `Overwrite`: dst contents `= {Null|Unknown}` or add. `Return`: each `r` in `contents(src)` → `= {Escaped}` if `isExactly(r)` else `|= {Escaped}`. `AddressEscape`: `contents(dst).mayUnknown = true`; each `r` → `|= {Escaped}`, `uncertain[r] = true`. `UnknownCall`: each `r` in each arg's contents → `|= {Released, Escaped}`, `uncertain[r] = true`. `Call`: for each `(loc, T)`: new = ∪ over states `st` in `contents(loc)`'s resources' state of `T[st]`; strong if `isExactly` else `|=`; if `T` maps any state to a set containing… (no uncertain unless `Certainty::Unknown` on the callee); `retDest`/`outArgs` with `Guaranteed` → `Acquire` semantics with `site = call.site`; `Conditional` → handled by the collector as edge events, never seen here; `Unknown` → `Acquire` + `uncertain`. `Exit`: no state change (solve records it).

`solve`: `in[0] = entry`, others ⊥; worklist in block order; process block: `out = in` then events; for each edge from block: `t = out`, apply edge events, `in[to].join(t)`; if changed push `to`. Exits are recorded on the **final** pass: after convergence, one more pass over all reached blocks replaying events and recording `Exit` events with the state at that point. Iteration limit `max(64, 16 * blocks)` or `iterationLimit`; on exhaustion `incomplete = true` and `exits` is left **empty**.

- [ ] **Step 1: Write failing tests** (synthetic facts; helper `Facts f = makeFacts(blocks, edges, locations, siteCount)`):
  - `Engine: acquire then exit leaves Owned` — block0: Acquire(site0→loc0), Exit → exit state resources[0] == {Owned}.
  - `Engine: release is strong on exact contents` — Acquire, Release(loc0), Exit → {Released}.
  - `Engine: conditional release joins to {Owned, Released}` — block0: Acquire; edges 0→1 (events: Release loc0) and 0→2; 1→3; 2→3; block3: Exit → {Owned, Released}.
  - `Engine: unreachable block stays bottom` — block with no incoming edge containing Acquire → its `out` not reached and resource still {NotOwned} at exit.
  - `Engine: return of exact contents escapes` — Acquire; Copy(retLoc ← loc0); Return(retLoc) → {Escaped}.
  - `Engine: return of overwritten slot keeps Owned` — Acquire; Copy(ret←loc0); edges 0→1 (Overwrite ret null) and 0→2; both → 3 Return(ret) → {Owned, Escaped}.
  - `Engine: alias keeps the old resource reachable` — Acquire(site0→h); Copy(saved←h); Acquire(site0→h) → after second acquire, `contents(saved) == {old(0)}` and resources[old] == {Owned}; then Release(saved), Release(h) → both Released.
  - `Engine: loop reacquire accumulates into old` — blocks: 0 entry → 1 (Acquire site0→h) → 1 (back-edge) and → 2 Exit → at exit `resources[old] has Owned` and `resources[new] has Owned`.
  - `Engine: address escape marks uncertain` — Acquire; AddressEscape(h); Exit → uncertain[new] true.
  - `Engine: call transformer release-always` — Acquire; Call{params: (h, T) with T[Owned] = {Released}}; Exit → {Released}; `release-sometimes` T[Owned] = {Owned, Released} → {Owned, Released}.
  - `Engine: exceptional exit before call effects` — block0: Acquire; Exit{exceptional} (inserted before Call by collector); Call(release); Exit → exits: exceptional state {Owned}, normal {Released}.
  - `Engine: budget exhaustion is explicit` — loop facts with `iterationLimit = 1` → `incomplete == true`, `exits.empty()`.
- [ ] **Step 2: Build** → fails: headers missing.
- [ ] **Step 3: Implement** `OwnershipFacts.hpp`, `OwnershipEngine.hpp/.cpp` per the semantics above.
- [ ] **Step 4: Build and run** → all `Engine:` PASS; `Domain:` still PASS.
- [ ] **Step 5: Commit** tests then implementation, `Refs #100`.

---

### Task 3: Summaries as transformers (engine side)

**Files:**
- Modify: `include/analysis/ownership/OwnershipEngine.hpp`, `src/analysis/ownership/OwnershipEngine.cpp`, `test/unit/ownership_engine_unit_tests.cpp`

**Interfaces — Produces:**

```cpp
// Runs solve once per (param, singleton) with entry state resources[paramResource] = singleton and
// location(paramLocation) = {paramResource}; image = join of that resource's state over exits of each kind.
// Fresh resources: `returns` = Guaranteed if on every normal exit the Return location isExactly a fresh
// resource, Conditional if on some, Unknown if uncertain; outArgs[i] likewise for ArgPointee(i) locations.
FunctionOwnershipSummary computeSummary(const OwnershipFacts& facts, unsigned iterationLimit = 0);
StateSet applyTransformer(const ParamTransformer& t, StateSet in);   // union of t[s] for s in in
ParamTransformer composeTransformers(const ParamTransformer& first, const ParamTransformer& then);
void joinTransformer(ParamTransformer& into, const ParamTransformer& other);
```

- [ ] **Step 1: Failing tests** — the four wrappers of spec §5 as facts with `paramResources = {(0, r)}`: always-release → `normal.params[0][Owned] == {Released}`; sometimes → `{Owned, Released}`; release-then-acquire (Release(loc0); Acquire(site→argPointee0)) → `{Released}` and `outArgs[0] == Guaranteed`; acquire-then-release → `{Owned}` and no outArgs. Plus `Summary: exceptional exit separates` (release only after an exceptional Exit → `exceptional.params[0][Owned] == {Owned}`, normal `{Released}`), `Summary: incomplete propagates` (`iterationLimit=1` on a loop → `incomplete`), `Summary: compose and join` (algebra on hand-built transformers).
- [ ] **Step 2: Build** → link error on `computeSummary`.
- [ ] **Step 3: Implement** in `OwnershipEngine.cpp`.
- [ ] **Step 4: Run** → PASS. **Step 5: Commit** (test, then impl).

---

### Task 4: Model grammar `if_ret…` and summary types in the public header

**Files:**
- Modify: `src/analysis/ResourceLifetimeAnalysis.cpp:76-84` (`ResourceRule` gains `RuleCondition condition`), `parseResourceModel` (lines ~293-385), `include/analysis/ResourceLifetimeAnalysis.hpp` (`ResourceSummaryFunction` gains `ownership::FunctionOwnershipSummary ownership;`; `ResourceLifetimeIssueKind::AnalysisIncomplete`; `ResourceLifetimeIssue` gains `unsigned exitLine = 0; bool certain = false;`)
- Test: `test/unit/analyzer_module_unit_tests.cpp` — parse a temp model file.

**Interfaces — Produces:** `enum class RuleCondition { Always, RetEqZero, RetNeZero, RetGeZero, RetLtZero, RetNeNull, RetEqNull };` and `bool parseResourceModelForTests(const std::string& path, std::vector<ParsedRuleForTests>&, std::string& error)` exposed in the header for the unit test (`ParsedRuleForTests { std::string pattern; unsigned argIndex; std::string kind; std::string action; RuleCondition condition; }`).

- [ ] **Step 1: Failing test** — write a temp model with `acquire_out f 0 K if_ret==0`, `acquire_ret g K if_ret!=null`, `release_arg h 0 K` → conditions parsed; `acquire_out f 0 K if_ret=0` → parse error mentioning line.
- [ ] **Step 2: Run** → fails (symbol missing). **Step 3: Implement** parsing of the optional 5th/4th token. **Step 4: Run** → PASS. **Step 5: Commit.**

---

### Task 5: Fact collector (locations, copies, returns, model calls, unknown calls)

**Files:**
- Create: `include/analysis/ownership/ResourceFactCollector.hpp`, `src/analysis/ownership/ResourceFactCollector.cpp` (add to CMake sources)
- Modify: `src/analysis/ResourceLifetimeAnalysis.cpp` — expose to the collector, via a small internal header `src/analysis/ResourceLifetimeInternal.hpp`, the existing `ResourceModel`, `ResourceRule`, `ruleMatchesFunction`, `resolveHandleStorage`, `resolvePointerStorage`, `StorageKey`, `describeMethodClass`, `resolveDirectCallee` (move their declarations; bodies stay).
- Test: `test/unit/analyzer_module_unit_tests.cpp` + input `test/unit/ownership_collector_input.c`.

**Interfaces — Produces:**

```cpp
namespace ctrace::stack::analysis::ownership {
struct CollectedFunction {
    OwnershipFacts facts;
    std::vector<const llvm::Instruction*> eventInstructions;        // by Event::instructionIndex
    std::vector<std::string> locationNames;                        // display names by LocationId
    std::vector<const llvm::Instruction*> siteInstructions;        // by site id
    std::vector<std::string> siteKinds;                            // resource kind by site id
    std::vector<const llvm::BasicBlock*> blockOf;                  // by block id
};
struct SummaryLookup { std::function<const FunctionOwnershipSummary*(const llvm::Function&)> byFunction; };
CollectedFunction collectOwnershipFacts(const llvm::Function& F, const ResourceModel& model,
                                        const SummaryLookup& summaries, const llvm::DataLayout& DL);
}
```

Collection rules: blocks in RPO (entry first); locations = one per `StorageKey` (scope Local → `Local`, strong if the alloca is only loaded/stored directly; Argument value → `NonLocal`; ThisField/Global → `NonLocal`; pointee of a pointer argument → `ArgPointee(i)`), plus one `Return` location, plus one location per pointer SSA value used as handle. Instruction mapping: `store v, p` with `v` pointer → `Copy(loc(p) ← loc(v))` if `v` is a handle location else `Overwrite(loc(p), unknownValue = !isa<ConstantPointerNull>(v))`; `load p` → `Copy(loc(load) ← loc(p))`; `phi` → per incoming edge, edge event `Copy(loc(phi) ← loc(incoming))`; `select` → `Copy(loc(sel) ← loc(a))` then `Copy(loc(sel) ← loc(b), strong = false)`; `ret v` → `Copy(Return ← loc(v))` + `Return(Return)` + `Exit{normal}`; call matching a rule: `Always` → `Acquire`/`Release` events in-block; `if_ret…` → find the unique `icmp` user of the return value feeding a `br` in the same block: emit the effect as an **edge event** on the successor where the predicate holds; otherwise emit with `Certainty::Unknown`; call to a function with a summary → `Call` event with `params` mapped through `mapSummaryEffectToCallerStorage`-style resolution of each argument to its location; call to an unmodelled callee: for each pointer argument that is a handle location → `UnknownCall`; for each argument that is the address of a local slot → `AddressEscape`, unless callee arg is `readonly`+`nocapture`/callee is `readnone`. Address-of-local escaping to a modelled acquire rule's out-arg is the `Acquire` dest, not an escape.

- [ ] **Step 1: Failing collector tests** on `ownership_collector_input.c` (functions: `early_return`, `alias_keep`, `select_return`, `unknown_call`): assert event kinds in order for each function (e.g. `early_return`: `Acquire`, then an edge with `Return`+`Exit` on the early path, `Release`, `Return`, `Exit`), and `select_return` yields two `Copy` into the select location with the second weak.
- [ ] **Step 2: Run** → fails. **Step 3: Implement** collector. **Step 4: Run** → PASS. **Step 5: Commit.**

---

### Task 6: Exceptions in the collector

**Files:** `src/analysis/ownership/ResourceFactCollector.cpp`, tests in `analyzer_module_unit_tests.cpp` with `test/unit/ownership_collector_input.cpp` (C++: `may_throw()`, `try/catch`, `noexcept`).

Rules (spec §7): the enclosing function is `mayUnwind = !F.doesNotThrow()`. For a `call` with `!CB.doesNotThrow()` and `mayUnwind`: emit `Exit{exceptional}` **before** the call's own events. For an `invoke`: no in-block call event; normal edge gets the callee's normal effects; unwind edge gets: summary `exceptional` transformer if present, else for model rules a weak version (`strong = false`, `Certainty::Conditional`) of release/`AcquireOut` effects, never `retDest`. `resume`, `cleanupret`/`catchswitch` unwinding to caller → `Exit{exceptional}`. `unreachable` → no exit.

- [ ] **Step 1: Failing tests** — `call_may_throw`: an exceptional `Exit` precedes the `Release`; `call_nothrow` (callee `noexcept`): no exceptional exit; `invoke_with_catch`: unwind edge carries weak release, normal edge strong; C input from Task 5 has **no** exceptional exits.
- [ ] **Step 2–5:** run red, implement, run green, commit.

---

### Task 7: Summary export/import, cross-TU v3

**Files:** `src/analysis/ResourceLifetimeAnalysis.cpp` (`buildResourceLifetimeSummaryIndex` computes `ownership` via `computeSummary(collectOwnershipFacts(...))` per function; `importExternalSummaryMap` keeps transformers; `computeChangedResourceFunctionNames` compares them), `src/app/AnalyzerApp.cpp` (`kCacheSchema = "cross-tu-resource-summary-v3"`, JSON `ownership: {normal:{params:{i:[b0,b1,b2,b3]}, returns, outArgs}, exceptional:{…}, incomplete}` in `writeSummaryCacheFile`/`readSummaryCacheFile`; a v2 file has no `ownership` key → treated as cache miss).
- Test: `run_test.py::check_resource_lifetime_cross_tu` already exists; add fixtures `test/resource-lifetime/cross-tu-wrapper-always-release-def.c` / `-use.c` (silence) and `cross-tu-wrapper-sometimes-release-def.c` / `-use.c` (partial leak) driven by a new `check_ownership_cross_tu()` following `check_resource_lifetime_cross_tu` (two runs, second from cache, identical output).

- [ ] **Step 1: Failing check + fixtures.** **Step 2: run red.** **Step 3: implement.** **Step 4: green.** **Step 5: commit.**

---

### Task 8: Diagnostics and switch-over of `MissingRelease`

**Files:** `src/analysis/ResourceLifetimeAnalysis.cpp` (replace the `MissingRelease` verdict loop at ~2984-3005 by: run collector + `solve`; if `incomplete` → one `AnalysisIncomplete` issue, skip; else replay every reached block in order for **reference loss** (before/after an `Acquire`/`Copy`/`Overwrite` on a strong location: for each `r` in before-contents(dst) not in after-contents and referenced by no other location after, with `Owned ∈ state(r)` and `!uncertain[r]`: issue at `siteInstructions[r/2]`, `certain = state.isOnly(Owned) && before.isExactly(r) && strong`), then **exit leaks** (for each exit and each `r` with `Owned ∈ state`, `!uncertain`: issue; `certain = every exit has isOnly(Owned)`; `exitLine` = line of the exit instruction if != function end line); one issue per (site, function), reference loss wins; remove the old `escapesViaReturn` / `localAddressEscapesToUnmodeledCall` suppression **for MissingRelease only**), `src/analyzer/DiagnosticEmitter.cpp` (messages: certain → existing text; possible → "potential resource leak: 'K' acquired in handle 'h' may leave the function without being released" + "↳ function exit at line N" when `exitLine`; reference loss → "… is overwritten at line N while still owned" / "may be overwritten…"; `AnalysisIncomplete` → Info as in #96).
- Fixtures (all TDD, each written and run red first): `early-return-leak.c`, `conditional-release-leak.c`, `release-all-paths-no-leak.c`, `conditional-acquire-no-release-leak.c`, `loop-reacquire-leak.c`, `reacquire-then-release-leak.c`, `reacquire-after-conditional-acquire-possible.c`, `loop-balanced-no-leak.c`, `alias-keeps-old-resource-no-leak.c`, rename `acquire-returned-conditional-no-leak.c` → `acquire-returned-conditional-leak.c` (expectation: possible leak), `return-slot-overwritten-leak.c`, `return-phi-leak.c`, `return-select-possible-leak.c`, `model-if-ret-failed-acquire-no-leak.c` (own model file under `test/resource-lifetime/models/`), `model-if-ret-untested-uncertain.c` (silence + `IncompleteInterproc`), `wrapper-always-release-no-leak.c`, `wrapper-sometimes-release-leak.c`, `wrapper-release-then-acquire.c`, `wrapper-acquire-then-release-no-leak.c`, `recursive-summary-no-leak.c`, `cpp-exception-paths.cpp`, `cpp-outparam-before-throw.cpp`, `cpp-call-may-throw-leak.cpp`, `cpp-nounwind-no-leak.cpp`, `c-no-exceptional-exit.c`, `incomplete-analysis-info.c` (drive via a `--resource-fixpoint-limit=1` hidden test option? **No** — the spec forbids new CLI; instead a unit test in `analyzer_module_unit_tests.cpp` calls `analyzeResourceLifetime` with the internal `fixpointIterationLimit` parameter, as done for #96).
- [ ] Steps: fixtures red → implement → green → run full suite → **list every existing `test/resource-lifetime/` fixture whose outcome changed, with cause, and stop for approval before editing any** → commit.

---

### Task 9: Determinism and final gate

- [ ] Run `python3 run_test.py --jobs 8 --no-cache` twice; diff the two logs (ignoring timings) → identical.
- [ ] Run `./build/ownership_engine_unit_tests` and `./build/stack_usage_analyzer_unit_tests .`.
- [ ] Update `docs/architecture/analyzer-modules.md` with the three components and `README.md` model grammar (`if_ret…`).
- [ ] Open PR `feature/resource-ownership-engine → main`, `Closes #100`, with the regression report.

---

## Self-review

- Spec §3.2 old/new per site → Task 2 `Acquire` semantics. §3.3 strong/weak → `Location::strongUpdatable` + `Contents::isExactly`. §4 events → Task 2 table; §4.1 grammar → Task 4, edge events → Task 5; §4.2 attributes → Task 5. §5 transformers → Task 3, cross-TU v3 → Task 7. §6 returns → Task 2 tests (`return of overwritten slot`) and Task 5 mapping. §7 exceptions → Task 6; convergence/incomplete → Task 2 + Task 8 `AnalysisIncomplete`. §8 diagnostics → Task 8. §9 compatibility → Task 8 stop-for-approval. §10 validation → Tasks 2, 3, 5, 6, 7, 8 tests; determinism → Task 9. §11 delivery → single PR from the integration branch (deviation from phase PRs, agreed in chat).
- Names consistent: `OwnershipFacts`, `solve`, `replay`, `applyEvent`, `computeSummary`, `collectOwnershipFacts`, `CollectedFunction`, `FunctionOwnershipSummary`, `ParamTransformer`.
