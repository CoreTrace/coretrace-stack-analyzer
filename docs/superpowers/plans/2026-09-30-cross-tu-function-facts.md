# Cross-TU Function Facts — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** For the six rules of the first table of #157, a call to a function defined in another file of the same run must reach the same conclusion as a call within one file. Four pull requests, with no result of a single-file run changed.

**Architecture:**

- Part 1: uninitialized summaries become keyed by the linker symbol, carry a completeness flag, and keep their empty entries. An incomplete summary counts as absent for its effects, and its status reaches its callers.
- Part 2: exports the const-pointee parameters of each definition.
- Part 3: computes two monotone fact sets, deterministic functions and size-minus-one wrapper pairs, in rounds where every module sees the same set.
- Part 4: builds one call graph over all loaded modules, and gives it unchanged to `computeRecursiveComponents` and `StackTotals` (#159).

**Tech Stack:** C++20, LLVM 20 (`llvm::Mangler`, `GlobalValue::hasExactDefinition`), Python 3 (`run_test.py`), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-29-cross-tu-function-facts-design.md`. The user validated it at c21f60c, with the corrections of fa01534: an incomplete summary is absent for its effects and stays incomplete.

## Global Constraints

**Order of work**

- **Prerequisite.** #170 (PR #171, merged as 2deb1f0) comes before Parts 1, 3 and 4, which read the IR rewritten by the never-return channel. Part 2 does not depend on it.
- **Level of detail.** Part 1 is written at code level, against `main` 2deb1f0. Parts 2 to 4 are written at interface level. Each is refined to code level when it starts, against the `main` that contains the previous parts, and that refinement is reviewed with its test proposal.
- **One PR per part.** Each part gets one PR against `main`, in English. It is assigned to SizzleUnrlsd, with the label `enhancement`, and its body ends with `Refs #157`; the last one ends with `Closes #157`. The user merges. After opening a PR, stop until the user reports the merge, then start the next part from an updated `main`.

**Tests**

- **TDD.** Write the test, run it and see it fail, implement, run it and see it pass.
- **Authorization.** No test file is created or modified before the user authorizes it. This covers fixtures, `run_test.py`, unit tests, their inputs, and the unit-test `CMakeLists.txt` entry. Each part presents its test changes with the evidence from `main` first (spec §9).

**Results**

- **Single-file results.** Every fixture analyzed alone gives the same functions and diagnostics before and after each part, in both passes (spec §4.5). Procedure M checks it.
- **Order independence.** Every multi-file result is independent of the order and names of the files (spec §4.4). The file pairs also run with renamed copies whose names invert the sort order.

**Commits**

- **Identity.** Commits use the repository's git identity, Hugo <hugo.payet@epitech.eu>, with no `Co-Authored-By` trailer and no mention of Claude.
- **Messages.** The subject is a conventional commit of at most 84 characters. The body ends with `Refs #157`.
- **Staging.** Never stage the user's untracked files (`.DS_Store`, `TODO.md`, `docs/architecture/inter-tu-analysis.md`, `pr-90-review.md`, `test-cedric/`). Stage by name, never with `git add -A` or `git add .`.

**Commands**

- **Format** each modified C/C++ file with `/opt/homebrew/opt/llvm@20/bin/clang-format -i <files>` (clang-format 20, as CI).
- **Build:** `cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8`.
- **Unit tests:** `./build/stack_usage_analyzer_unit_tests . && ./build/ownership_engine_unit_tests`.
- **Fixtures:** `python3 run_test.py --jobs 8 --no-cache`.
- **Push** over HTTPS: `git -c credential.helper= -c credential.helper='!gh auth git-credential' push -q https://github.com/CoreTrace/coretrace-stack-analyzer.git <branch>`.
- **Code scanning.** After CI, triage the code-scanning alerts as `CLAUDE.md` says: new alerts only, each reproduced locally. Fix the true positives. Report any false positive to the user, and never dismiss one to get a green PR. Add the triage to the PR body.

## Review Focus

These inputs follow from the spec but no listed test exercises them. They are ordered from the most likely to bite first, and each has its test in the owning part.

1. **A caller that defines the callee itself.** The in-module definition must win, and the imported fact must never be consulted. Test in Task 1.4, `run_test.py` case "own definition".
2. **C++ overloads.** `read_only(const int*)` and `read_only(int*)` in two files are two symbols: the const fact of one must not reach calls to the other. Test in Part 2.
3. **An unprototyped declaration `int read_only();`** called with one argument. The argument count at the call must match the definition, or the fact is absent. Test in Part 2.
4. **A deterministic function that calls `abort()`.** It stays deterministic, as within one file, because a `noreturn` declaration is accepted. Test in Part 3.
5. **A cycle over three files where one file has a `static` homonym of a member.** The homonym gets no edge. Test in Part 4.

## Tooling and Procedure M (local, never committed)

`SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad`. Define it in each command; shell state does not persist.

**Baseline.** `$SCRATCH/wt-main-base`, detached at the `main` the part starts from and built with `stack_usage_analyzer`. Update it at the start of each part:

```sh
git -C $SCRATCH/wt-main-base checkout -q --detach origin/main
cmake --build $SCRATCH/wt-main-base/build --target stack_usage_analyzer -j 8
```

**Procedure M**, run at the end of each part with `BASE=$SCRATCH/wt-main-base/build/stack_usage_analyzer` and `FIX=<part worktree>/build/stack_usage_analyzer`:

1. **Fixtures alone.** `python3 $SCRATCH/probe-152/compare_all_units.py $BASE $FIX <part worktree>/test`. Expected: `0 differ` and `0 failed` in both passes. The new fixtures are part of the run: both binaries analyze the same files.
2. **Lua 5.4.8, all files together.** `python3 /Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer/.superpowers/sdd/2026-09-24-smt-solver-precision/compare_together.py $BASE $FIX $SCRATCH/corpora/lua-5.4.8/src lvm.c`. Every difference is explained in the PR.
3. **The first table of #157.** `python3 $SCRATCH/repro-157/run.py $FIX`. The part's row now reports in two files what it reports in one; the other rows are unchanged.
4. **Part 4 only.** `python3 $SCRATCH/probe-152/compare_stacks.py $BASE $FIX <part worktree>/test`, and the same on `$SCRATCH/probe-152/lua-src` with `--compile-arg=-I$SCRATCH/corpora/lua-5.4.8/src`.

## File Structure

| Part | Files |
|---|---|
| 1 | `include/analysis/AnalyzerUtils.hpp`, `src/analysis/AnalyzerUtils.cpp` (linker symbol); `include/analysis/UninitializedVarAnalysis.hpp`, `src/analysis/UninitializedVarAnalysis.cpp` (keys, completeness, empty entries); `src/app/CrossTUSummaryDriver.hpp` (cycle cap); `src/app/AnalyzerApp.cpp` (definitions, multiply defined symbols); tests |
| 2 | `src/analysis/ConstParamAnalysis.cpp` and its header; `include/StackUsageAnalyzer.hpp` (config field); `src/app/AnalyzerApp.cpp`; `src/analyzer/AnalysisPipeline.cpp`; tests |
| 3 | `src/analysis/DuplicateIfCondition.cpp`, `src/analysis/SizeMinusKWrites.cpp` and their headers; `include/StackUsageAnalyzer.hpp`; `src/app/AnalyzerApp.cpp`; `src/analyzer/AnalysisPipeline.cpp`; tests |
| 4 | `include/analysis/StackComputation.hpp`, `src/analysis/StackComputation.cpp` (resolver); `src/analyzer/ModulePreparationService.cpp`; `include/StackUsageAnalyzer.hpp`; `src/app/AnalyzerApp.cpp`; tests |

---

## Part 1 — `UninitializedLocalRead`: "writes nothing" crosses files (spec §5)

Branch `157-uninitialized-no-effect`, worktree `$SCRATCH/wt-157-1`, from `main` 2deb1f0.

### Task 1.1: Worktree and baseline

- [ ] **Step 1: Create the worktree**

```sh
M=/Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
git -C $M -c credential.helper= -c credential.helper='!gh auth git-credential' fetch -q https://github.com/CoreTrace/coretrace-stack-analyzer.git main:refs/remotes/origin/main
git -C $M worktree add -q -b 157-uninitialized-no-effect $SCRATCH/wt-157-1 origin/main
cd $SCRATCH/wt-157-1 && cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DZ3_DIR=/opt/homebrew/lib/cmake/z3 -DENABLE_Z3_BACKEND=ON > ../wt-157-1.configure.log 2>&1 \
  && cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8 > ../wt-157-1.build.log 2>&1; echo "exit $?"
```

Expected: `exit 0`, `git log --oneline -1` shows 2deb1f0 or a later `main`.

### Task 1.2: Test proposal, evidence, authorization

Nothing in this task touches the worktree. The proposed files are written to `$SCRATCH/proposal-157-1/`, mirroring the repository paths.

**Files proposed:**

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-def.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// peek() for cross-tu-uninitialized-noeffect-use.c (#157): it writes nothing through p.

void peek(int* p)
{
    (void)p;
}
```

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-use.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// Caller of peek() (#157). Alone, peek() is an unknown void function, presumed to write through
// p: nothing is reported. With cross-tu-uninitialized-noeffect-def.c, peek() writes nothing and
// the read of v is reported, as within one file. run_test.py checks each pairing.

void peek(int* p);

int uses_peek(void)
{
    int v;
    peek(&v);
    return v;
}
```

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-weak.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// A weak peek() that writes nothing (#157): the linker may retain another definition, so it
// proves nothing about the one that runs.

__attribute__((weak)) void peek(int* p)
{
    (void)p;
}
```

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-again.c`, a second definition that writes nothing, and `test/uninitialized-variable/cross-tu-uninitialized-noeffect-writes.c`, a second definition that writes. They have the same header comment, stating that a symbol defined by several files never proves "writes nothing" (spec §5.3). Their bodies are `(void)p;` and `*p = 1;`.

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-static.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// A static peek() (#157). It belongs to this file only: it is not a definition of the peek() that
// cross-tu-uninitialized-noeffect-use.c calls, and must not hide the one of
// cross-tu-uninitialized-noeffect-def.c. Within this file, it writes nothing: the read is reported.

static void peek(int* p)
{
    (void)p;
}

int local_peek(void)
{
    int v;
    peek(&v);
    return v;
}
```

  It gets the expectation block that `check_file` needs for the read of `v` in `local_peek`. The block is written from the actual output on `main`.

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-asm-use.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// Calls peek() under another C name, through an assembler name (#157): it is the same symbol for
// the linker, on Linux as on macOS, so the summary of cross-tu-uninitialized-noeffect-def.c
// applies.

#define STR2(x) #x
#define STR(x) STR2(x)

void peek_under_another_name(int* p) __asm__(STR(__USER_LABEL_PREFIX__) "peek");

int uses_asm_name(void)
{
    int v;
    peek_under_another_name(&v);
    return v;
}
```

- Create `test/uninitialized-variable/cross-tu-uninitialized-abi-def.cpp` and `test/uninitialized-variable/cross-tu-uninitialized-abi-use.cpp`. `canonicalizeMangledName` gives both symbols below the same key, although they are distinct ABI symbols; the linker key must keep them apart.

```cpp
// abi-def.cpp
namespace std { inline namespace __1 { struct tag; } }
namespace probe
{
    void peek(int* p, std::tag* t) { (void)p; (void)t; }   // _ZN5probe4peekEPiPNSt3__13tagE
}
```

```cpp
// abi-use.cpp
namespace std { inline namespace __cxx11 { struct tag; } }
namespace probe
{
    void peek(int* p, std::tag* t);                        // _ZN5probe4peekEPiPNSt7__cxx113tagE
}
int uses_other_abi() { int v; probe::peek(&v, nullptr); return v; }
```

  Both files are formatted as the other fixtures, with an SPDX header and a comment giving the two mangled names.

- Create `test/uninitialized-variable/cross-tu-uninitialized-noeffect-own.c` for Review Focus 1. It defines its own `peek()`, which writes, and calls it. Analyzed with `cross-tu-uninitialized-noeffect-def.c`, its own definition wins: nothing is reported.

- Modify `run_test.py`: add `check_uninitialized_cross_tu_no_effect()` and register it after `check_uninitialized_cross_tu`. It is modeled on `check_noreturn_cross_tu`: JSON, both passes, a case table, and renamed copies in a `tempfile.TemporaryDirectory` for the inverted sort order. Rule watched: `UninitializedLocalRead`. File names below drop the `cross-tu-uninitialized-` prefix.

| Case | Files | Expected functions |
|---|---|---|
| use alone | `noeffect-use.c` | none |
| def and use | `noeffect-def.c noeffect-use.c` | `uses_peek` |
| inverted sort order | the same, renamed so that use sorts first | `uses_peek` |
| weak def | `noeffect-weak.c noeffect-use.c` | none |
| defined twice, neither writes | `noeffect-def.c noeffect-again.c noeffect-use.c` | none |
| defined twice, one writes | `noeffect-def.c noeffect-writes.c noeffect-use.c` | none |
| static homonym | `noeffect-def.c noeffect-static.c noeffect-use.c` | `local_peek`, `uses_peek` |
| assembler name | `noeffect-def.c noeffect-asm-use.c` | `uses_asm_name` |
| other ABI symbol | `abi-def.cpp abi-use.cpp` | none |
| own definition | `noeffect-def.c noeffect-own.c` | none |

- Create `test/unit/uninit_incomplete_a.c`, `test/unit/uninit_incomplete_b.c` and `test/unit/uninit_incomplete_c.c`, the three modules of spec §5.4:

```c
// uninit_incomplete_a.c: built with a one-iteration limit, so no analysis converges.
void f(int* p) { (void)p; }
int fi(int* p) { (void)p; return 0; }
```

```c
// uninit_incomplete_b.c: built with the normal budget and the index of module A.
void f(int* p);
int fi(int* p);
int use_f(void) { int v; f(&v); return v; }
int use_fi(void) { int v; fi(&v); return v; }
void gv(int* p) { fi(p); }
void gv2(int* p) { gv(p); }
void h(int* p) { (void)p; }
```

```c
// uninit_incomplete_c.c: built with the normal budget and the index of modules A and B.
void gv2(int* p);
int use_gv2(void) { int v; gv2(&v); return v; }
```

  Each file gets the SPDX header and a comment saying what it provides, in the style of `test/unit/uninit_fixpoint_budget_input.c`.

- Modify `test/unit/analyzer_module_unit_tests.cpp`:
  - add `testLinkerSymbolName` (Task 1.3), `testUninitializedSummaryKeysAreLinkerSymbols` (Task 1.4), `testUninitializedIncompleteSummaries` (Task 1.5) and `testCrossTUDriverMarksUnconvergedCycles` (Task 1.5), each registered in `main()` after `testUninitializedFixpointBudgetIsExplicit`;
  - change the two lookups `summaries.functions.find("fill")` of `testUninitializedFixpointBudgetIsExplicit` (lines 643 and 671) to `summaries.functions.find(linkerSymbolName(*loaded.module->getFunction("fill")))`. The key becomes the linker symbol, which on macOS is `_fill`.
- Modify `CMakeLists.txt`: add `target_include_directories(stack_usage_analyzer_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)`. The driver header lives in `src/app/` and is not public.

- [ ] **Step 1: Write the proposal files** under `$SCRATCH/proposal-157-1/` with the content above. Write the expectation block of `noeffect-static.c` from the analyzer's actual output on `main`.
- [ ] **Step 2: Evidence on `main`, fixtures alone.** Call `check_file` of `run_test.py` on each new fixture, importing `run_test.py` with `RUN_CONFIG.analyzer` set to the baseline binary. This is the harness used for #170. Expected: every fixture passes.
- [ ] **Step 3: Evidence on `main`, the new check.** Run `check_uninitialized_cross_tu_no_effect()` from the proposed `run_test.py` against the baseline binary, with `RUN_CONFIG.test_dir` pointing to a copy of `test/` that contains the proposed fixtures.
  - Expected to fail: "def and use", "inverted sort order", "static homonym" (`uses_peek` missing) and "assembler name".
  - Expected to pass: the other cases. They guard against a wrong implementation: "other ABI symbol" would fail with canonical keys, and "defined twice, neither writes" would fail without the multiply-defined rule.
- [ ] **Step 4: Evidence on `main`, the three modules.** Build the scratch harness `$SCRATCH/harness-157/harness2` against the baseline, and run it on the three unit inputs. Expected on `main`:
  - index of A: empty;
  - issues of B: `use_fi.v`, with or without the index of A;
  - issues of C: none, with or without the index of A and B.

  These equalities must hold after the change too, now that empty summaries are kept. The unit tests that read `complete` do not compile on `main`: the field does not exist yet.
- [ ] **Step 5: Present the proposal to the user.** List the files, the per-case expectations and the evidence of Steps 2 to 4, and stop. Continue only after the user authorizes the test changes.

### Task 1.3: `linkerSymbolName`

**Files:**
- Modify: `include/analysis/AnalyzerUtils.hpp`, `src/analysis/AnalyzerUtils.cpp`
- Test: `test/unit/analyzer_module_unit_tests.cpp` (`testLinkerSymbolName`)

**Interfaces:**
- Produces: `std::string ctrace::stack::analysis::linkerSymbolName(const llvm::GlobalValue& GV)`. It is used by Tasks 1.4 and 1.5 and by Parts 2 to 4.

- [ ] **Step 1: Write the failing test.** Add the authorized test to `test/unit/analyzer_module_unit_tests.cpp`, with `#include "analysis/AnalyzerUtils.hpp"` and `#include <llvm/AsmParser/Parser.h>`:

```cpp
    bool testLinkerSymbolName(TestReport& report)
    {
        using ctrace::stack::analysis::linkerSymbolName;
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        const auto parse = [&](const std::string& dataLayout)
        {
            return llvm::parseAssemblyString("target datalayout = \"" + dataLayout + "\"\n"
                                             "declare void @plain()\n"
                                             "declare void @\"\\01verbatim\"()\n"
                                             "declare void @_ZN5probe4peekEPNSt3__13tagE()\n"
                                             "declare void @_ZN5probe4peekEPNSt7__cxx113tagE()\n",
                                             error, context);
        };
        const std::unique_ptr<llvm::Module> elf = parse("e-m:e-i64:64-n32:64-S128");
        const std::unique_ptr<llvm::Module> macho = parse("e-m:o-i64:64-n32:64-S128");
        if (!elf || !macho)
        {
            report.expect(false, "LinkerSymbolName setup: the IR does not parse");
            return false;
        }
        report.expect(linkerSymbolName(*elf->getFunction("plain")) == "plain",
                      "LinkerSymbolName: ELF adds no prefix");
        report.expect(linkerSymbolName(*macho->getFunction("plain")) == "_plain",
                      "LinkerSymbolName: Mach-O adds its underscore");
        report.expect(linkerSymbolName(*macho->getFunction("\1verbatim")) == "verbatim",
                      "LinkerSymbolName: an assembler name is taken as written");
        report.expect(linkerSymbolName(*elf->getFunction("_ZN5probe4peekEPNSt3__13tagE")) !=
                          linkerSymbolName(*elf->getFunction("_ZN5probe4peekEPNSt7__cxx113tagE")),
                      "LinkerSymbolName: std::__1 and std::__cxx11 symbols stay distinct");
        return report.failures == 0;
    }
```

- [ ] **Step 2: Build and run.** Expected: a compile error, `linkerSymbolName` is not declared.
- [ ] **Step 3: Implement.** In `include/analysis/AnalyzerUtils.hpp`, add `class GlobalValue;` to the forward declarations of `namespace llvm`, and declare:

```cpp
    /// The symbol the linker resolves for @p GV: its IR name with the target's global prefix, or an
    /// assembler name (`__asm__`, an IR name starting with '\1') taken as written. Facts cross
    /// files only between functions with the same linker symbol (#157).
    std::string linkerSymbolName(const llvm::GlobalValue& GV);
```

  In `src/analysis/AnalyzerUtils.cpp`, add `#include <llvm/ADT/SmallString.h>` and `#include <llvm/IR/Mangler.h>`, and:

```cpp
    std::string linkerSymbolName(const llvm::GlobalValue& GV)
    {
        llvm::SmallString<64> name;
        llvm::Mangler().getNameWithPrefix(name, &GV, /*CannotUsePrivateLabel=*/false);
        return std::string(name);
    }
```

- [ ] **Step 4: Build and run the unit tests.** Expected: the four `LinkerSymbolName` assertions pass, and the whole binary reports no `[FAIL]`.
- [ ] **Step 5: Commit** the unit test and the implementation together, as `feat(analysis): name functions across files by their linker symbol` with `Refs #157`.

### Task 1.4: Linker-symbol keys for uninitialized summaries

**Files:**
- Modify: `include/analysis/UninitializedVarAnalysis.hpp`, `src/analysis/UninitializedVarAnalysis.cpp`, `src/app/AnalyzerApp.cpp`
- Test: `testUninitializedSummaryKeysAreLinkerSymbols`, and the two changed lookups of `testUninitializedFixpointBudgetIsExplicit`

**Interfaces:**
- Consumes: `linkerSymbolName` (Task 1.3).
- Produces: summary index keys equal to `linkerSymbolName(F)`. `getCanonicalCalleeNames` is renamed `getCalleeSymbolNames`.

- [ ] **Step 1: Write the failing test** (authorized):

```cpp
    bool testUninitializedSummaryKeysAreLinkerSymbols(const std::filesystem::path& repoRoot,
                                                      TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/uninit_fixpoint_budget_input.c", config,
                                  loaded, loadError))
        {
            report.expect(false, "UninitSummaryKeys setup: " + loadError);
            return false;
        }
        const UninitializedSummaryIndex index = buildUninitializedSummaryIndex(
            *loaded.module, [](const llvm::Function&) { return true; },
            static_cast<const UninitializedSummaryIndex*>(nullptr));
        const std::string key = linkerSymbolName(*loaded.module->getFunction("fill"));
        report.expect(index.functions.count(key) == 1,
                      "UninitSummaryKeys: the summary of fill is keyed by its linker symbol");
        return report.failures == 0;
    }
```

- [ ] **Step 2: Build and run.** Expected on macOS: `[FAIL] UninitSummaryKeys`, because the key is `fill`, not `_fill`, together with the two changed lookups of `testUninitializedFixpointBudgetIsExplicit`. On Linux the prefix is empty and this test passes before the change. It must still pass after, and the `run_test.py` cases "assembler name" and "other ABI symbol" of Task 1.5 cover the difference there.
- [ ] **Step 3: Implement**, in `src/analysis/UninitializedVarAnalysis.cpp`:
  - `#include "analysis/AnalyzerUtils.hpp"`; drop `#include "mangle.hpp"` if nothing else uses it.
  - Rename `CanonicalCalleeNameMap` to `CalleeSymbolNameMap`, `buildCanonicalCalleeNameMap` to `buildCalleeSymbolNameMap`, and the member and parameters `canonicalCalleeNames` to `calleeSymbolNames`.
  - `buildCalleeSymbolNameMap`: `names.try_emplace(callee, linkerSymbolName(*callee));`
  - `transferInstruction`: the fallback lookup becomes `externalSummariesByName->find(linkerSymbolName(*callee))`.
  - `exportSummaryIndexForModule`: `out.functions[linkerSymbolName(F)] = exportPublicFunctionSummary(normalized);`
  - `importExternalSummaryMap`: `out.emplace(entry.first, std::move(summary));`. The keys are already linker symbols.
  - Rename the public function `getCanonicalCalleeNames` to `getCalleeSymbolNames`, in the header too.

  In `src/app/AnalyzerApp.cpp`:

```cpp
// Definitions by symbol key. A static function is no definition of an external symbol (#157).
static std::unordered_map<std::string, std::vector<std::size_t>>
collectModuleDefinitions(const std::vector<LoadedInputModule>& loadedModules,
                         const std::function<std::string(const llvm::Function&)>& keyOf)
{
    std::unordered_map<std::string, std::vector<std::size_t>> definitions;
    for (std::size_t i = 0; i < loadedModules.size(); ++i)
        for (const llvm::Function& function : *loadedModules[i].module)
            if (!function.isDeclaration() && !function.hasLocalLinkage() && function.hasName() &&
                !function.getName().empty())
                definitions[keyOf(function)].push_back(i);
    return definitions;
}
```

  - The resource channel calls it with `[](const llvm::Function& f) { return ctrace_tools::canonicalizeMangledName(f.getName().str()); }`, and keeps its key (spec §11).
  - The uninitialized channel calls it with `[](const llvm::Function& f) { return analysis::linkerSymbolName(f); }`, and uses `analysis::getCalleeSymbolNames`.
  - Leaving `static` definitions out also changes the resource plan, but only when a `static` homonym exists.
- [ ] **Step 4: Build, run the unit tests and the fixtures.** Expected: no `[FAIL]`, and `run_test.py` still passes, since no empty summary crosses files yet.
- [ ] **Step 5: Commit** as `refactor(uninitialized): key summaries by the linker symbol` with `Refs #157`.

### Task 1.5: Completeness, empty summaries, incomplete summaries as absent

**Files:**
- Modify: `include/analysis/UninitializedVarAnalysis.hpp`, `src/analysis/UninitializedVarAnalysis.cpp`, `src/app/CrossTUSummaryDriver.hpp`, `src/app/AnalyzerApp.cpp`
- Test: the authorized fixtures and `check_uninitialized_cross_tu_no_effect`; `testUninitializedIncompleteSummaries`, `testCrossTUDriverMarksUnconvergedCycles`

**Interfaces:**
- Produces: `UninitializedSummaryFunction::complete`. Empty summaries of exact definitions are exported, merged and imported. The driver calls `operations.markIncomplete(Index&)` on every module of a cyclic group that did not converge, when the operations define it.

- [ ] **Step 1: Add the authorized fixtures, the `run_test.py` check, the unit inputs and the two unit tests.**

`testUninitializedIncompleteSummaries` loads the three unit inputs with `loadModuleFromSource`, then:

```cpp
        auto analyzeAll = [](const llvm::Function&) { return true; };
        const auto entry = [](const UninitializedSummaryIndex& index, const llvm::Module& module,
                              const char* name) -> const UninitializedSummaryFunction*
        {
            const auto it = index.functions.find(linkerSymbolName(*module.getFunction(name)));
            return it == index.functions.end() ? nullptr : &it->second;
        };
        const auto reads = [](const std::vector<UninitializedLocalReadIssue>& issues)
        {
            std::set<std::string> names;
            for (const UninitializedLocalReadIssue& issue : issues)
                if (issue.kind == UninitializedLocalIssueKind::ReadBeforeDefiniteInit)
                    names.insert(issue.funcName);
            return names;
        };

        const UninitializedSummaryIndex indexA = buildUninitializedSummaryIndex(
            *a.module, analyzeAll, static_cast<const UninitializedSummaryIndex*>(nullptr),
            /*fixpointIterationLimit=*/1);
        for (const char* name : {"f", "fi"})
        {
            const UninitializedSummaryFunction* summary = entry(indexA, *a.module, name);
            report.expect(summary && !summary->complete,
                          std::string("UninitIncomplete: ") + name + " is exported, incomplete");
        }

        const UninitializedSummaryIndex indexB =
            buildUninitializedSummaryIndex(*b.module, analyzeAll, &indexA);
        for (const char* name : {"gv", "gv2"})
        {
            const UninitializedSummaryFunction* summary = entry(indexB, *b.module, name);
            report.expect(summary && !summary->complete,
                          std::string("UninitIncomplete: ") + name +
                              " is incomplete through an intermediate function");
        }
        const UninitializedSummaryFunction* h = entry(indexB, *b.module, "h");
        report.expect(h && h->complete, "UninitIncomplete: h is complete, its empty summary kept");

        const std::set<std::string> readsB =
            reads(analyzeUninitializedLocalReads(*b.module, analyzeAll, &indexA));
        report.expect(readsB == reads(analyzeUninitializedLocalReads(*b.module, analyzeAll, nullptr)),
                      "UninitIncomplete: B reports as if no summary were imported");
        report.expect(readsB.count("use_fi") == 1 && readsB.count("use_f") == 0,
                      "UninitIncomplete: int fi() keeps the report of an unknown declaration");

        UninitializedSummaryIndex indexAB = indexA;
        (void)mergeUninitializedSummaryIndex(indexAB, indexB);
        const std::set<std::string> readsC =
            reads(analyzeUninitializedLocalReads(*c.module, analyzeAll, &indexAB));
        report.expect(readsC == reads(analyzeUninitializedLocalReads(*c.module, analyzeAll, nullptr)),
                      "UninitIncomplete: C reports as if no summary were imported");
        report.expect(readsC.count("use_gv2") == 0,
                      "UninitIncomplete: no caller reads the empty summary of gv2 as no write");
        return report.failures == 0;
```

`testCrossTUDriverMarksUnconvergedCycles` includes `"app/CrossTUSummaryDriver.hpp"`. It runs `runCrossTUSummaryPass` on two modules that call each other, which form one cyclic group, with fake operations:

```cpp
        struct Entry
        {
            int value = 0;
            bool complete = true;
        };
        struct Operations
        {
            using Index = std::map<std::string, Entry>;
            using External = int;
            bool converges = false;
            mutable int round = 0;
            int marked = 0;
            External prepareLevel(const Index&) const { return 0; }
            External prepareCycle(const Index&) const { return 0; }
            Index build(std::size_t module, const Index&, const External&) const
            {
                return {{module == 0 ? "a" : "b", {converges ? 1 : ++round, true}}};
            }
            bool tryCache(std::size_t, const External&, Index&) const { return false; }
            void cache(std::size_t, const External&, const Index&) const {}
            static void merge(Index& into, const Index& from)
            {
                for (const auto& [name, entry] : from)
                {
                    auto [it, inserted] = into.emplace(name, entry);
                    if (!inserted)
                        it->second = {entry.value, it->second.complete && entry.complete};
                }
            }
            static bool equals(const Index& a, const Index& b)
            {
                return a.size() == b.size() &&
                       std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y)
                                  { return x.first == y.first && x.second.value == y.second.value &&
                                           x.second.complete == y.second.complete; });
            }
            static std::unordered_set<std::string> changedNames(const Index&, const Index& b)
            {
                std::unordered_set<std::string> names;
                for (const auto& entry : b)
                    names.insert(entry.first);
                return names;
            }
            void markIncomplete(Index& index)
            {
                ++marked;
                for (auto& entry : index)
                    entry.second.complete = false;
            }
            void reportIteration(std::size_t, unsigned, bool, std::size_t) const {}
            void reportLimit(std::size_t, unsigned) const {}
            void reportLevel(std::size_t, const CrossTUSummaryPlan::Level&, std::int64_t) const {}
        };
```

  - **Never converges.** The pass must end with `marked == 2` and both entries of the global index incomplete.
  - **Converges** (`converges = true`). The pass must end with `marked == 0` and both entries complete.
  - **Serial `parallelFor`.** `[](const std::vector<std::size_t>& items, auto&& fn) { for (std::size_t i : items) fn(i); }`.

- [ ] **Step 2: Build and run.** Expected: a compile error, `complete` and `markIncomplete` do not exist yet. Then, with the header change of Step 3 alone, `[FAIL]` lines for `UninitIncomplete` and for the driver test, and `check_uninitialized_cross_tu_no_effect` failing on the cases listed in Task 1.2, Step 3.

- [ ] **Step 3: Implement the completeness flag.** In `include/analysis/UninitializedVarAnalysis.hpp`:

```cpp
    struct UninitializedSummaryFunction
    {
        std::vector<UninitializedSummaryParamEffect> paramEffects;
        // False when the analysis that produced this summary stopped early, or used an
        // incomplete summary. A caller then takes it as absent for its effects, and becomes
        // incomplete in turn (#157).
        std::uint64_t complete : 1 = true;
        std::uint64_t reservedFlags : 63 = 0;
    };
```

  In `src/analysis/UninitializedVarAnalysis.cpp`, give `FunctionSummary` the same two bit-fields, and compare them in its `operator==`:

```cpp
            bool operator==(const FunctionSummary& other) const
            {
                return paramEffects == other.paramEffects && complete == other.complete;
            }
```

  The equality makes the existing caller-dirty loop of `computeFunctionSummaries` carry the flag from callee to caller within a module.

  In `analyzeFunction`, summary mode:

```cpp
                if (!converged)
                {
                    downgradeWriteClaims(*outSummary);
                    outSummary->complete = false;
                }
```

  In `computeFunctionSummaries`, after the loop:

```cpp
            // Summaries still moving at the round cap are no fixpoint: none is complete (#157).
            if (changed)
                for (auto& entry : summaries)
                    entry.second.complete = false;
```

  In `transferInstruction`, remember where the callee summary came from, and apply the rule of spec §5.3:

```cpp
            bool imported = false;
            // ... in the external lookup:
            //     calleeSummary = &itExternal->second;
            //     imported = true;
            if (calleeSummary && !calleeSummary->complete)
            {
                // The caller is no more complete than what it used. Imported, an incomplete
                // summary counts as absent for its effects: the call gets the presumption for
                // declarations, exactly as an unknown function (#157).
                if (currentSummary)
                    currentSummary->complete = false;
                if (imported)
                    calleeSummary = nullptr;
            }
            const bool hasSummary = (calleeSummary != nullptr);
```

  Within one module an incomplete summary is used as today, and only its flag spreads (spec §4.5).

- [ ] **Step 4: Keep empty summaries, and carry the flag.** In `src/analysis/UninitializedVarAnalysis.cpp`:
  - `exportSummaryIndexForModule`: replace `if (normalized.paramEffects.empty()) continue;` with the following. An empty summary says "writes nothing" only from an exact definition (spec §4.2):

```cpp
                if (normalized.paramEffects.empty() && !F.hasExactDefinition())
                    continue;
```

  - `exportPublicFunctionSummary`: `out.complete = summary.complete;`.
  - `importPublicFunctionSummary` and `importExternalSummaryMap`: copy `complete`, and keep the empty entries. Drop the `if (!summary.paramEffects.empty())` guard.
  - `mergeUninitializedSummaryIndex`: drop `if (srcSize == 0) continue;`.
  - `mergePublicFunctionSummary`: add

```cpp
            if (!src.complete && dst.complete)
            {
                dst.complete = false;
                changed = true;
            }
```

  - `publicFunctionSummaryEquals`: return `false` when `lhs.complete != rhs.complete`.

- [ ] **Step 5: The driver and the app.** In `src/app/CrossTUSummaryDriver.hpp`, in `runCrossTUSummaryPass`:

```cpp
                if (!converged)
                {
                    operations.reportLimit(scc.size(), kMaxSCCIterations);
                    // Summaries still moving at the cap are no fixpoint (#157).
                    if constexpr (requires(Index& index) { operations.markIncomplete(index); })
                        for (std::size_t module : scc)
                            operations.markIncomplete(moduleSummaries[module]);
                }
```

  In `src/app/AnalyzerApp.cpp`, `UninitializedSummaryOperations` gains:

```cpp
    // Symbols defined by several modules: a caller may see one definition before the others,
    // so none of their summaries proves "writes nothing" (#157).
    std::unordered_set<std::string> multiplyDefined;

    static void markIncomplete(Index& index)
    {
        for (auto& entry : index.functions)
            entry.second.complete = false;
    }
```

  - `build` marks every entry whose key is in `multiplyDefined` incomplete before returning the index.
  - `buildCrossTUUninitializedSummaryIndex` fills `multiplyDefined` from `definedBy`, with every key that has more than one module.

- [ ] **Step 6: Build and run** the unit tests and the fixtures. Expected: no `[FAIL]`, `check_uninitialized_cross_tu_no_effect` passes all its cases in both passes, and the full suite passes.
- [ ] **Step 7: Commit.**
  - Commit the authorized tests first, as `test(uninitialized): an empty summary crosses files only when it proves no write`.
  - Then commit the change, as `feat(uninitialized): carry "writes nothing" across files, never from an incomplete analysis`.
  - Both bodies end with `Refs #157`.

### Task 1.6: Validate, measure, report, open the PR

- [ ] **Step 1: Full suite.** `sh /Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer/.superpowers/sdd/2026-09-24-smt-solver-precision/suite.sh` from the worktree. Expected: `all green`.
- [ ] **Step 2: Procedure M**, points 1 to 3.
  - Fixtures alone: `0 differ`.
  - Lua together: every change explained. A change can only come from a callee defined in another Lua file whose summary is empty, complete and exact.
  - First table of #157: the `UninitializedLocalRead` row reports `uses_peek` in two files.
- [ ] **Step 3: Self-analysis** of each modified `.cpp`, before and after, with `--compile-commands=build/compile_commands.json`. Compare the diagnostics by function and message.
- [ ] **Step 4: Push the branch and open the PR.** Title: `feat(uninitialized): carry "writes nothing" across files, never from an incomplete analysis`. Label `enhancement`, assignee SizzleUnrlsd. The body has these sections:
  - problem;
  - change;
  - tests, with a before/after table per case;
  - effect (Procedure M);
  - limits: an in-module non-converged summary is still used as is (#168); inexact definitions keep today's behavior; the resource channel keeps its key;
  - `Refs #157`.
- [ ] **Step 5: CI and code scanning.** Triage them (Global Constraints) and add the triage to the body. Report to the user, in French, with:
  - the results before and after;
  - the limits;
  - the commit messages;
  - the merge and cleanup commands.

---

## Part 2 — `ConstParameterNotModified`: the parameter types of the definition (spec §6)

Refined to code level when it starts. It does not depend on #170. The interfaces:

**Fact computation**

- In `src/analysis/ConstParamAnalysis.cpp`, extract the type test of `calleeParamIsReadOnly` into a helper, `bool parameterPointeeIsConst(const llvm::Function& definition, unsigned index)`. It keeps the same exclusions: pointer to pointer, `void*`, function pointer.
- Export `using ConstPointeeParams = std::unordered_map<std::string, std::vector<bool>>;` and `ConstPointeeParams collectConstPointeeParams(const llvm::Module&)`. The key is the linker symbol, and the vector gives, for each parameter, whether it is declared const-pointee. Only exact external definitions that are not variadic are collected.
- New field in `AnalysisConfig`: `std::shared_ptr<const ConstPointeeParams> constPointeeParams`.

**Publication.** `analyzeWithSharedModuleLoading` collects the vectors of every module after the never-return step. It publishes a symbol only if every definition of it is exact, has the same parameter count and the same vector (spec §6.2, §4.2).

**Consumer.** In `callArgWriteState`, a callee that is a declaration without debug information gets `NoWrite` for a parameter when three conditions hold:
- the published vector marks that parameter const-pointee;
- the call passes as many arguments as the vector has entries;
- the declaration is not variadic.

**Tests to propose.** They go in `test/pointer_reference-const_correctness/`, with a new JSON check in `run_test.py`:
- a pair reported alone and together;
- negative cases: a non-const parameter, an argument count that differs (Review Focus 3), a `static` homonym;
- C++ overloads, `read_only(const int*)` and `read_only(int*)` (Review Focus 2);
- a symbol defined twice, const and not const, in both sort orders;
- the inverted sort order.

**Acceptance.** The `ConstParameterNotModified` row of the #157 table reports in two files, and Procedure M finds 0 difference on the fixtures alone.

## Part 3 — `DuplicateIfCondition` and `SizeMinusOneWrite`: two transitive facts (spec §7)

Refined to code level when it starts. It requires #170. The interfaces:

**Determinism**

- `std::set<std::string> deterministicDefinitions(llvm::Module&, const std::set<std::string>& deterministicElsewhere)` in `DuplicateIfCondition`. It returns the linker symbols of the module's exact external definitions that are deterministic, when the declarations named in the set count as deterministic.
- The process-wide determinism cache, a `static` keyed by `Function*`, is cleared at each call.
- `isFunctionDeterministic` and the call-site test consult the set for declarations. The definition of determinism does not change otherwise (spec §7.1).

**Size-minus-one pairs**

- `using SizeMinusKWrapperPairs = std::map<std::string, std::set<std::pair<unsigned, unsigned>>>;`
- `SizeMinusKWrapperPairs sizeMinusKWrapperPairs(llvm::Module&, const SizeMinusKWrapperPairs& elsewhere)` in `SizeMinusKWrites`. Pass 2 of `buildSizeMinusKSummaries` also follows declarations, through `elsewhere`.
- At a call to a declaration, the published pairs apply when the argument count matches.

**Configuration.** New fields `std::shared_ptr<const std::set<std::string>> deterministicFunctions` and `std::shared_ptr<const SizeMinusKWrapperPairs> sizeMinusKWrapperPairs`.

**Publication.** `analyzeWithSharedModuleLoading` runs one loop per fact, on the model of the #170 loop.
- It first counts the non-static definitions of each symbol.
- Each round examines every module against the same set.
- A symbol is published only when every definition of it has the fact. For pairs, only the pairs common to every definition are published.
- The loop stops when a round publishes nothing (spec §7.3).

**Tests to propose**, in `test/diagnostics/` and `test/size-arg/`, with JSON checks:
- pairs of files;
- chains over three files;
- a cycle across two files;
- negative cases: a read of a mutable global, an unknown call, a `static` homonym;
- "every definition", for each fact, with a control case, in both sort orders;
- the inverted sort order;
- a deterministic function that calls `abort()` (Review Focus 4).

**Acceptance.** Both rows of the #157 table report in two files, and Procedure M finds 0 difference on the fixtures alone.

## Part 4 — Recursion and max stack: one call graph for all files (spec §8)

Refined to code level when it starts. It requires #170. The interfaces:

**Resolver.** In `StackComputation.hpp`, `struct CallResolver` with `const llvm::Function* resolve(const llvm::Function& callee) const`. It returns the callee when it is defined. Otherwise it returns the definition in another loaded module whose linker symbol is the callee's, when that definition is exact and unique, and `nullptr` if there is none.

**Local stacks and recursion**

- `computeLocalStack(llvm::Function&, const llvm::DataLayout&, AnalysisMode, const CallResolver* = nullptr)`. A resolved call is not counted by `isUnresolvedCall`, and it counts in `hasNonSelfCall`, as a call to a definition in the same file does.
- `detectInfiniteRecursionComponent(component, config, const CallResolver* = nullptr)`. The test for a recursive call resolves declarations first, and `leavesThroughNoreturnCall` receives the same test (spec §8.4, choice B1).

**Global state**

- `analyzeWithSharedModuleLoading` builds it after the never-return step and after `runFunctionAttrsPass` on every module, so that the global graph sees the same IR as a one-file run.
- The graph has the in-module edges, plus one edge for each resolved declaration.
- `LocalStack` and `Order` cover every function: modules in sorted order, then functions in module order.
- It runs `computeGlobalStackUsage` and `computeRecursiveComponents` unchanged (#159), then the infinite-recursion test with the resolver.
- The result goes to a new field `std::shared_ptr<const InternalAnalysisState> globalStackState`, keyed by `const llvm::Function*`. The modules stay loaded, so these pointers stay valid.

**Consumer.** When `cfg.globalStackState` is set, `ModulePreparationService` reads `TotalStack`, `RecursiveFuncs` and `InfiniteRecursionFuncs` of its functions from it, instead of computing them per module.

**Tests to propose**, in `test/recursion/`, with a JSON check modeled on `check_cycle_max_stack`:
- a recursion pair, reported `Detected`;
- the two cycles of spec §8.4: without a way out, `Unconditional` on both members; with a possible way out, `Detected` only;
- a stack pair (`big_frame`);
- a cycle across files, whose members and callers are unknown, with a bound;
- a `static` homonym in each file (Review Focus 5);
- a symbol defined twice, which gets no edge;
- ABI mode;
- `--assume-external-frame`;
- the inverted sort order.

**Acceptance.** Both rows of the #157 table report in two files. Procedure M finds 0 difference on the fixtures alone, and `compare_stacks.py` explains every change on Lua. The PR closes #157.

**Limit to report.** `strongConnect` stays recursive (alert #227), and its depth grows with the longest call chain of the whole project.

## Self-review against the spec

| Spec | Where |
|---|---|
| §4.1 absent, empty, incomplete; incompleteness reaches callers | Task 1.5, Steps 3 and 5; tests A/B/C and the driver |
| §4.2 linker symbol, `static`, exact definitions, every definition before publication, signature | Tasks 1.3 and 1.4; Task 1.5, Steps 4 and 5; Parts 2 to 4, "Publication" |
| §4.3 meaning of facts | Part 2, fact computation (interface contract); Part 3, determinism (definition unchanged) |
| §4.4 convergence and order | Part 3, publication (same set per round); Part 4 (#159); inverted sort order in every part |
| §4.5 one file unchanged | Procedure M, point 1, in every part; Task 1.5, Step 3 (in-module use unchanged) |
| §5 to §8, the four PRs | Parts 1 to 4 |
| §9 validation, test authorization | Task 1.2; "Tests to propose" in Parts 2 to 4; Global Constraints |
| §10 choices A1 and B1 | Part 2, consumer (contract only); Part 4, recursion (B1) |
