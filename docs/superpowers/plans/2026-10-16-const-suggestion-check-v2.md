# Const Suggestion Check v2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix `ParameterDebugBinding` on `main` (P0), then rework T, the experimental Clang check of `ConstParameterNotModified` suggestions, so that each verdict is justified and tied to the exact candidate, units and configurations it checked.

**Architecture:** P0 binds each IR argument to its own debug declaration: its type, whether the source declares it, and its position among the source parameters. T v2 then runs a fixed flow on `217-t-v2`: initial compilation and planning in the defining file, initial and modified compilations of each unit that sees the change, correspondences established from the AST, a seven-step judgement per unit, aggregation, report. Verdicts are verified, refuted or indeterminate; the IR comparator only gains the established pairs as an input.

**Tech Stack:** C++20, LLVM and Clang 20 (IR, AST, CodeGen `GlobalDecl`), `run_test.py` fixtures and checks, `stack_usage_analyzer_unit_tests`.

**Spec:** `docs/superpowers/specs/2026-10-16-const-suggestion-check-v2-design.md`, approved at `8b283a8`.

**Level of detail.** P0 is planned step by step with its code, validated in a throwaway prototype (evidence below). Stages 1 to 3 and the final phase are planned to the level of tasks, files, interfaces, tests and validation criteria. The step-level code of a stage is added to this plan when that stage starts: P0 and the updated `main` are integrated first, and they change the code these stages modify. A stage starts only once the previous one is validated.

**Evidence:** `coretrace-stack-analyzer-evidence/217/p0/` (`repro/`, `harness/`, `fixture/`, `effect/`, `prototype.diff`, `ir/`).

## Global Constraints

From the spec:
- T runs only with `--experimental-const-suggestion-check`. Without the option T changes nothing (§4); P0 is the only change to the rule itself, and Task P0.2 measures it.
- Three verdicts (§3): verified (kept, with the "checked …" line), refuted (removed from the public diagnostics), indeterminate (kept, with "not checked with Clang: <reason>"). An IR difference that R, O and K do not explain is indeterminate, never refuted.
- Refuted only in a unit whose plan scope is validated, on an introduced compile error, an introduced tracked warning, or a changed R, O or K fact (§5.8, steps 4 to 6).
- Facts are the closed list of §5.4: R (the declaration a call or a reference targets, unevaluated operands and instantiations included), O (what each method named like the entity overrides), K (each `if constexpr` condition, each `_Generic` association).
- Correspondences come from compilation facts only, per unit, between its initial and modified compilations. Heuristics (`withoutConst`, `variantOf`, masked encodings, template keys) have no verdict power (§5.7, §5.9).
- T never assumes that files are linked together and never decides ODR; collisions follow the table of §5.10.
- One candidate per suggestion, built once by the planner, checked and displayed (§3; D12, D13).
- Tracked warning groups: `-W<group>`, then `-Wno-error=<group>`, in the initial and modified compilations (§5.3, stage 2).
- The lines tied to `reliesOnCalleeConst` (#167) stay unchanged.
- Cost criterion: +10 % on the self-analysis, same binary without and with the option, order without, with, with, without (§8). Not accepted in advance.
- Out of scope: several configurations per file, a compilation shared with coretrace-compiler, a persistent cache, link groups, a rework of the comparator.

T v1 texts stay verbatim, since tests pin them, except the changes that §6 and §7 list:
- `checked in the analyzed files only, with their compile commands: `, `checked in every file of the compilation database that sees the change, with its compile command: `, the sentence that follows them, `not checked with Clang: `;
- the reasons `a file does not compile with Clang as it is`, `<file> does not compile with Clang as it is`, `<file> cannot be preprocessed with Clang`, `the declaration is written by a macro definition`, `no source file, so whether the suggested type compiles was not verified`, `<file> uses the function but has no source`, `the rewritten declaration does not get the suggested type`;
- the trace line `[const-suggestion] <function> parameter <name>: <text>`, with `removed, does not compile: <file>:<line>:` and `removed, gains a checked warning: <file>:<line>:` (`check_const_suggestion_removal_reason`). New trace fields go on further `[const-suggestion]` lines.

Process:
- TDD. A new test, fixture, `run_test.py` check or unit test, and a changed expectation, are presented with their failure evidence and added only once the user authorizes them.
- Public actions (issue, push, pull request, comment, body edit, alert dismissal) only on the user's explicit go. Bodies are prepared as files and passed with `--body-file`.
- Commits authored by Hugo, conventional subjects, no co-author, no attribution line. `CLAUDE.md` is never committed.
- The user merges. #221 stays a draft and is never merged. T v1 stays an immutable reference: `217-t-v1` at `2a5b0c1`, `coretrace-stack-analyzer-evidence/217/archive/t-v1-2a5b0c1.bundle`.
- Worktrees under `coretrace-stack-analyzer-work/`; evidence under `coretrace-stack-analyzer-evidence/217/`, never in `/tmp`. The untracked files of the main checkout stay as they are.
- Shared Linux machines (WSL): long runs under `nohup`; never signal a process that is not ours; other jobs, containers and images stay as they are.

Commands (macOS):
- Configure: `cmake -S . -B build -G Ninja -DLLVM_DIR=$(brew --prefix llvm@20)/lib/cmake/llvm -DClang_DIR=$(brew --prefix llvm@20)/lib/cmake/clang -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DBUILD_ANALYZER_UNIT_TESTS=ON`
- Build: `cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8`
- Unit tests: `./build/stack_usage_analyzer_unit_tests . && ./build/ownership_engine_unit_tests`
- Suite: `python3 run_test.py --jobs 8 --no-cache` (build with Z3)
- Format: `$(brew --prefix llvm@20)/bin/clang-format -i <files>` (20.1.2)

## Review Focus

1. **Windows targets (Microsoft C++ ABI).** A constructor of a class with a virtual base takes `is_most_derived` after the source parameters; `main` types it as the last source parameter and reports it (`double *is_most_derived`). It must be artificial, and the other parameters keep their types. Pinned in Task P0.1 (unit test, `--target=x86_64-pc-windows-msvc`).
2. **C++23 explicit object parameter.** Clang marks `self` an object pointer, not artificial, and the source declares it: in `pick(this const Picker& self, int* p)`, `p` has source index 1, and T rewrites `p`. Pinned in Task P0.1 (unit test) and Task 1.2 (`const-suggestion-explicit-object.cpp` unchanged).
3. **A function defined in a header that several analyzed files include** (`static inline` in C, `inline` in C++): one entity in each unit, never a collision with itself, verified when each unit verifies it. Pinned in Task 1.6 (new `run_test.py` check).
4. **A file that the compilation database lists twice, with different commands.** The loader keeps one; T checks only that configuration, never claims the other, and the trace names it. Pinned in Task 1.1 (new `run_test.py` check).
5. **`-Werror=<group>` on a tracked group**, not plain `-Werror`: the group stays a warning in both compilations, and an introduced warning still refutes. Pinned in Task 2.1 (fixture variant).

---

## File structure

P0, from `main`:

| File | Responsibility |
|---|---|
| `include/analysis/ParameterDebugBinding.hpp` | `ParameterDebugBinding::sourceIndex`. |
| `src/analysis/ParameterDebugBinding.cpp` | Entry in the subroutine type past the parameters it does not list; artificial from the parameter's own declaration; source index. |
| `test/unit/analyzer_module_unit_tests.cpp` | `testParameterDebugBinding`. |
| `test/unit/parameter_binding_input.cpp`, `test/unit/parameter_binding_block_input.c` | Its inputs. |
| `test/pointer_reference-const_correctness/virtual-base-vtt.cpp` | What the rule reports. |

T v2, on `217-t-v2`, starting from T v1's files:

| File | Responsibility |
|---|---|
| `src/analysis/ConstSuggestionCheck.cpp` | Flow (§5, steps 1 to 5), units and their configurations, judgement, aggregation, report, trace. |
| `src/analysis/ConstSuggestionPlan.cpp` (new) | Planning (§5.5): entity and source identity, parameter, candidate, edits, plan scope. |
| `src/analysis/ConstSuggestionFacts.cpp` (new) | Inspection of one compilation (§5.3, §5.4, §5.6): declarations, R, O and K facts and their keys, defined symbols and linkage, explicit address casts, diagnostics, scope after the rewrite. |
| `src/analysis/ConstSuggestionMatch.cpp` (new) | Correspondences (§5.7) and collisions (§5.10). |
| `src/analysis/ConstSuggestionCode.cpp` | Comparator; one new input, the established pairs (§5.9). |
| `src/analysis/ConstSuggestionInternal.hpp` | Types the files above share. |
| `include/analysis/ConstSuggestionCheck.hpp` | `checkConstSuggestions` unchanged; `rewrittenCodeDifference` takes the pairs. |
| `src/analyzer/DiagnosticEmitter.cpp` | Lines of the three verdicts; displayed candidate (D12, D13). |
| `README.md` | D3 sentence corrected; limits of §10. |

T v1's `ConstSuggestionCheck.cpp` holds 1 267 lines; the split follows the responsibilities of §5, which require no class or interface of their own.

---

## Stage P0 — `ParameterDebugBinding`, its own issue and pull request from `main`

Root cause, at `main` `d1d9271`. At `-O0`, `buildDebugParamLayout` indexes the function's `DISubroutineType` with the debug number (`arg:`) of the argument's spilled declaration. That number counts every parameter; the subroutine type lists `this` and a block's descriptor, but not what the C++ ABI adds: the `vtt` of a base-object constructor (C2) of a class with a virtual base, after `this`, or `is_most_derived` under the Microsoft ABI, after the parameters. Artificiality is read from the entry found there, not from the parameter's own declaration. Hence:
- `vtt` gets the type of the first source parameter, and is reported (`int *vtt`);
- the parameters after it get the type of the next one (`double *x` for `int* x`);
- a constructor that calls such a C2 trusts the shifted type as a `const` contract (#167): `Leaf::Leaf(int* p, …) : Writer(p, …)` reports `p` as "passed where a pointer to const is expected" although `Writer` writes through it;
- `is_most_derived` (an `int`) is reported as `double *is_most_derived`;
- a block's `.block_descriptor` is reported as `<unknown type> *.block_descriptor`.

Reproductions: `evidence/217/p0/repro/` (`main-d1d9271.txt`, `caller-main-d1d9271.txt`, `block-main-d1d9271.txt`).

### Task P0.1: bind each parameter to its own declaration

**Files:**
- Create: `test/unit/parameter_binding_input.cpp`, `test/unit/parameter_binding_block_input.c`
- Create: `test/pointer_reference-const_correctness/virtual-base-vtt.cpp`
- Modify: `test/unit/analyzer_module_unit_tests.cpp` (includes, new test, its call in `main`)
- Modify: `include/analysis/ParameterDebugBinding.hpp`, `src/analysis/ParameterDebugBinding.cpp`

**Interfaces:**
- Consumes: `loadModuleForAnalysis` through the unit tests' `loadModuleFromSource`; `AnalysisConfig::extraCompileArgs`.
- Produces, used by Task 1.2:

```cpp
struct ParameterDebugBinding
{
    // ...existing fields...
    // The parameter's position among those the source declares, from 0. None for one the
    // source does not declare (`this`, vtt, a block's descriptor), or that -O0 code does not
    // spill.
    std::optional<unsigned> sourceIndex;
    // ...existing fields...
};
```

- [ ] **Step 1: Open the issue and the branch.** After the user's go: an issue labelled `bug`, titled `ConstParameterNotModified: the vtt of a base-object constructor is reported and shifts the parameters after it`, body prepared in `evidence/217/p0/issue-body.md` (root cause and reproductions above). Then:

```bash
R=/Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer
git -C $R fetch origin
git -C $R worktree add /Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer-work/wt-<N>-binding -b <N>-parameter-debug-binding origin/main
```

- [ ] **Step 2: Write the unit test inputs.**

`test/unit/parameter_binding_input.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
// Parameters for resolveParameterDebugBinding: a free function, a method, the constructors of a
// class with a virtual base, and a method with an explicit object parameter. Compiled with
// -std=c++23, once for each C++ ABI: under the Itanium ABI, the base-object constructor (C2)
// takes a vtt after `this`; under the Microsoft ABI, the constructor takes is_most_derived last.

struct Base
{
    int seed;
};

struct Reader : virtual Base
{
    Reader(int* x, double* y);
    long sum(int* p, double* q);
};

Reader::Reader(int* x, double* y)
{
    seed = *x + static_cast<int>(*y);
}

long Reader::sum(int* p, double* q)
{
    return *p + static_cast<long>(*q);
}

long free_function(int* a, double* b)
{
    return *a + static_cast<long>(*b);
}

// An explicit object parameter is declared in the source: p is the second parameter.
struct Picker
{
    int pick(this const Picker& self, int* p)
    {
        return *p;
    }
};

int use_picker(int* p)
{
    return Picker{}.pick(p);
}
```

`test/unit/parameter_binding_block_input.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// A block's invoke function takes the block first, as `.block_descriptor`: an artificial
// parameter that, unlike vtt, the debug type of the function lists. Compiled with -fblocks.

int call_block(int (^block)(int*, double*), int* a, double* b)
{
    return block(a, b);
}

int use_block(int* a, double* b)
{
    return call_block(^(int* x, double* y) { return *x + (int)*y; }, a, b);
}
```

- [ ] **Step 3: Add the unit test, without source indexes.** In `test/unit/analyzer_module_unit_tests.cpp`, add `#include "analysis/ParameterDebugBinding.hpp"` after `#include "analysis/IntRanges.hpp"`, and `#include <llvm/IR/DebugInfoMetadata.h>` after `#include <llvm/IR/Constants.h>`. Add, in the anonymous namespace, after `testCompileIRCacheWithoutCompilerRevision`:

```cpp
    /// resolveParameterDebugBinding: each parameter gets its own type. The parameters the source
    /// does not declare are artificial: `this`, the vtt that the base-object constructor (C2) of a
    /// class with a virtual base takes, is_most_derived under the Microsoft ABI, a block's
    /// descriptor.
    bool testParameterDebugBinding(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const auto describe = [](const llvm::Function& F)
        {
            std::vector<std::string> parameters;
            for (const llvm::Argument& Arg : F.args())
            {
                const ParameterDebugBinding binding = resolveParameterDebugBinding(F, Arg);
                std::string text = binding.name + ": ";
                if (binding.isArtificial)
                {
                    text += "artificial";
                }
                else
                {
                    const auto* pointer = llvm::dyn_cast_or_null<llvm::DIDerivedType>(binding.type);
                    const llvm::DIType* pointee = pointer ? pointer->getBaseType() : nullptr;
                    text += (pointee ? pointee->getName().str() : "?") + " *";
                }
                parameters.push_back(text);
            }
            return parameters;
        };
        const auto check = [&](const LoadedModule& loaded, const char* symbol,
                               const std::vector<std::string>& expected)
        {
            const llvm::Function* F = loaded.module->getFunction(symbol);
            const std::vector<std::string> actual = F ? describe(*F) : std::vector<std::string>{};
            std::string text;
            for (const std::string& parameter : actual)
                text += " [" + parameter + "]";
            report.expect(actual == expected,
                          std::string("ParameterDebugBinding: ") + symbol + text);
        };
        const auto load =
            [&](const char* input, std::vector<std::string> compileArgs, LoadedModule& out)
        {
            ctrace::stack::AnalysisConfig config;
            config.extraCompileArgs = std::move(compileArgs);
            std::string loadError;
            if (loadModuleFromSource(repoRoot / "test/unit" / input, config, out, loadError))
                return true;
            report.expect(false,
                          "ParameterDebugBinding setup: failed to load module: " + loadError);
            return false;
        };

        LoadedModule itanium;
        LoadedModule microsoft;
        LoadedModule blocks;
        if (!load("parameter_binding_input.cpp", {"-std=c++23"}, itanium) ||
            !load("parameter_binding_input.cpp", {"-std=c++23", "--target=x86_64-pc-windows-msvc"},
                  microsoft) ||
            !load("parameter_binding_block_input.c", {"-fblocks"}, blocks))
            return false;
        check(itanium, "_Z13free_functionPiPd", {"a: int *", "b: double *"});
        check(itanium, "_ZN6Reader3sumEPiPd", {"this: artificial", "p: int *", "q: double *"});
        check(itanium, "_ZN6ReaderC1EPiPd", {"this: artificial", "x: int *", "y: double *"});
        check(itanium, "_ZN6ReaderC2EPiPd",
              {"this: artificial", "vtt: artificial", "x: int *", "y: double *"});
        check(itanium, "_ZNH6Picker4pickERKS_Pi", {"self: artificial", "p: int *"});
        check(microsoft, "??0Reader@@QEAA@PEAHPEAN@Z",
              {"this: artificial", "x: int *", "y: double *", "is_most_derived: artificial"});
        check(blocks, "__use_block_block_invoke",
              {".block_descriptor: artificial", "x: int *", "y: double *"});

        // What the rule displays for x does not depend on the constructor variant.
        const llvm::Function* complete = itanium.module->getFunction("_ZN6ReaderC1EPiPd");
        const llvm::Function* base = itanium.module->getFunction("_ZN6ReaderC2EPiPd");
        if (complete && base)
        {
            const ParameterDebugBinding inComplete =
                resolveParameterDebugBinding(*complete, *complete->getArg(1));
            const ParameterDebugBinding inBase =
                resolveParameterDebugBinding(*base, *base->getArg(2));
            report.expect(inComplete.name == inBase.name && inComplete.type == inBase.type &&
                              inComplete.line == inBase.line &&
                              inComplete.column == inBase.column &&
                              inComplete.confidence == inBase.confidence,
                          "ParameterDebugBinding: x binds alike in C1 and C2");
        }
        return report.failures == 0;
    }
```

In `main`, after `(void)testLinkerSymbolName(report);`, add `(void)testParameterDebugBinding(repoRoot, report);`.

- [ ] **Step 4: Add the fixture.** `test/pointer_reference-const_correctness/virtual-base-vtt.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
// The base-object constructor (C2) of a class with a virtual base takes a vtt after `this`: a
// parameter that the source does not declare and the debug type of the function does not list.
// ConstParameterNotModified must not report it, nor give a parameter after it the type of the
// next one, in that constructor or in the constructors that call it.

struct Base
{
    int seed;
};

struct Reader : virtual Base
{
    Reader(int* x, double* y);
};

// x is only read: both variants, C1 and C2, report it with its own type.
// not contains: current type: double *x
// at line 24, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'Reader::Reader(int*, double*)' is never used to modify the pointed object
//          ↳ current type: int *x
//          ↳ suggested type: const int *x
Reader::Reader(int* x, double* y)
{
    seed = *x;
    *y = 0;
}

// Writer writes through p, so Leaf, which passes its own p there, must not report it.
struct Writer : virtual Base
{
    Writer(int* p, const double* q);
};

struct Leaf : Writer
{
    Leaf(int* p, const double* q);
};

Writer::Writer(int* p, const double* q)
{
    *p = static_cast<int>(*q);
}

// not contains: parameter 'p' in function 'Leaf::Leaf(int*, double const*)'
Leaf::Leaf(int* p, const double* q) : Writer(p, q) {}

// not contains: parameter 'vtt'
```

`// not contains:` lines must not follow an `// at line` block directly: `run_test.py` would read them as part of the block.

- [ ] **Step 5: Build and run, expect RED.**

Run: the build command, `./build/stack_usage_analyzer_unit_tests . 2>&1 | grep ParameterDebugBinding`, then `python3 run_test.py --jobs 8 --no-cache 2>&1 | grep -A3 virtual-base-vtt`.

Expected (recorded on `d1d9271` in `evidence/217/p0/harness/red-unit-runtime-main.txt` and `evidence/217/p0/fixture/red-fixture-main.txt`):

```
[FAIL] ParameterDebugBinding: _ZN6ReaderC2EPiPd [this: artificial] [vtt: int *] [x: double *] [y: double *]
[FAIL] ParameterDebugBinding: ??0Reader@@QEAA@PEAHPEAN@Z [this: artificial] [x: int *] [y: double *] [is_most_derived: double *]
[FAIL] ParameterDebugBinding: __use_block_block_invoke [.block_descriptor: ? *] [x: int *] [y: double *]
[FAIL] ParameterDebugBinding: x binds alike in C1 and C2
```

The free function, the method, C1 and `Picker::pick` pass. The fixture finds its three forbidden texts (`current type: double *x`, Leaf's `p`, `vtt`) in both passes, default and smt-z3.

- [ ] **Step 6: Commit.**

```bash
git add test/unit/analyzer_module_unit_tests.cpp test/unit/parameter_binding_input.cpp test/unit/parameter_binding_block_input.c test/pointer_reference-const_correctness/virtual-base-vtt.cpp
git commit -m "test(const-param): an artificial parameter is neither reported nor shifts the others"
```

- [ ] **Step 7: Bind each parameter to its own declaration.** In `src/analysis/ParameterDebugBinding.cpp`:

Add `#include <algorithm>` before `#include <cstdint>`. Replace `DebugParamLayout` with:

```cpp
        struct DebugParamLayout
        {
            const llvm::DISubroutineType* subroutineType = nullptr;
            bool hasCanonicalDebugIndex = false;
            bool isHiddenAbiArg = false;
            // Make alignment intent explicit before the 32-bit indexes.
            std::uint16_t paddingBeforeDebugArgIndex = 0;
            unsigned debugArgIndex = 0; // The debug parameter number (arg:): `this` and vtt count.
            unsigned typeIndex = 0;     // Its entry in the subroutine type, which omits vtt.
            std::uint32_t paddingTail = 0;
        };
```

Add before `buildDebugParamLayout`:

```cpp
        // The entry of @p param in the subroutine type, or 0 when the type does not list it.
        // Clang marks artificial the parameters that the source does not declare. The type lists
        // `this` and a block's descriptor, but not what the C++ ABI adds after `this`, like the vtt
        // of a constructor of a class with a virtual base: the artificial parameters other than
        // `this` are unlisted when the type lacks exactly that many entries.
        static unsigned subroutineTypeIndex(const llvm::Function& F,
                                            const llvm::DILocalVariable& param, std::size_t listed)
        {
            const auto artificialOtherThanThis = [](const llvm::DILocalVariable& var)
            { return var.isArtificial() && !var.isObjectPointer(); };
            unsigned last = 0;
            unsigned artificial = 0;
            unsigned artificialBefore = 0;
            for (const llvm::Argument& other : F.args())
            {
                const llvm::DILocalVariable* var = spilledParameter(other);
                if (!var)
                    continue;
                last = std::max(last, var->getArg());
                if (!artificialOtherThanThis(*var))
                    continue;
                ++artificial;
                if (var->getArg() < param.getArg())
                    ++artificialBefore;
            }
            if (artificial == 0 || last != listed + artificial)
                return param.getArg();
            return artificialOtherThanThis(param) ? 0 : param.getArg() - artificialBefore;
        }
```

In `buildDebugParamLayout`, replace the spilled branch with:

```cpp
            if (const llvm::DILocalVariable* spilled = spilledParameter(Arg);
                spilled && spilled->getArg() != 0)
            {
                const unsigned typeIndex = subroutineTypeIndex(F, *spilled, typeArray.size() - 1);
                if (typeIndex == 0)
                {
                    layout.isHiddenAbiArg = true;
                    return layout;
                }
                if (typeIndex < typeArray.size())
                {
                    layout.debugArgIndex = spilled->getArg();
                    layout.typeIndex = typeIndex;
                    layout.hasCanonicalDebugIndex = true;
                    return layout;
                }
            }
```

and, at the end of the function, after `layout.debugArgIndex = debugParamOffset + 1;`, add `layout.typeIndex = layout.debugArgIndex;`.

In `resolveParameterDebugBinding`, replace `if (layout.isHiddenAbiArg)` with:

```cpp
        // The declaration of the spilled parameter says whether the source declares it.
        const llvm::DILocalVariable* spilled = spilledParameter(Arg);
        if (layout.isHiddenAbiArg || isSyntheticDebugVariable(spilled))
```

and in the fallback on the subroutine type, index `typeArray` with `layout.typeIndex` instead of `layout.debugArgIndex` (the bound check and the subscript).

- [ ] **Step 8: Build and run, expect GREEN.** The eight `ParameterDebugBinding` checks pass; the fixture passes in both passes; `./build/stack_usage_analyzer_unit_tests .` ends with `All analyzer module unit tests passed.`

- [ ] **Step 9: Commit.**

```bash
git add src/analysis/ParameterDebugBinding.cpp
git commit -m "fix(const-param): bind each parameter to its own debug declaration"
```

- [ ] **Step 10: Expect source indexes in the unit test.** In `describe`, after the `if`/`else`, add:

```cpp
                text += ", source " +
                        (binding.sourceIndex ? std::to_string(*binding.sourceIndex) : "none");
```

Replace the expectations with:

```cpp
        check(itanium, "_Z13free_functionPiPd", {"a: int *, source 0", "b: double *, source 1"});
        check(itanium, "_ZN6Reader3sumEPiPd",
              {"this: artificial, source none", "p: int *, source 0", "q: double *, source 1"});
        check(itanium, "_ZN6ReaderC1EPiPd",
              {"this: artificial, source none", "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZN6ReaderC2EPiPd",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZNH6Picker4pickERKS_Pi",
              {"self: artificial, source 0", "p: int *, source 1"});
        check(microsoft, "??0Reader@@QEAA@PEAHPEAN@Z",
              {"this: artificial, source none", "x: int *, source 0", "y: double *, source 1",
               "is_most_derived: artificial, source none"});
        check(blocks, "__use_block_block_invoke",
              {".block_descriptor: artificial, source none", "x: int *, source 0",
               "y: double *, source 1"});
```

and the comment above the test with:

```cpp
    /// resolveParameterDebugBinding: each parameter gets its own type, and its position among the
    /// parameters the source declares. The others are artificial: `this`, the vtt that the
    /// base-object constructor (C2) of a class with a virtual base takes, is_most_derived under the
    /// Microsoft ABI, a block's descriptor.
```

`self` stays artificial, as on `main`: Clang marks it an object pointer, which the binding has always treated as synthetic. Its source index is 0 all the same, since the source declares it.

- [ ] **Step 11: Build, expect RED.** Expected: `error: no member named 'sourceIndex' in 'ctrace::stack::analysis::ParameterDebugBinding'` (twice; recorded in `evidence/217/p0/harness/red-unit-compile-main.txt`).

- [ ] **Step 12: Give each parameter its source index.** In `include/analysis/ParameterDebugBinding.hpp`, add `#include <optional>` after `#include <cstdint>`, and after `unsigned column = 0;`:

```cpp
        // The parameter's position among those the source declares, from 0. None for one the
        // source does not declare (`this`, vtt, a block's descriptor), or that -O0 code does not
        // spill.
        std::optional<unsigned> sourceIndex;
```

The struct keeps no implicit padding: `std::optional<unsigned>` takes 8 bytes at a 4-byte boundary, after `column`.

In `src/analysis/ParameterDebugBinding.cpp`, add `#include <optional>` after `#include <limits>`, and after `isSyntheticDebugVariable`:

```cpp
        // The position of @p param among the parameters the source declares: the artificial ones
        // before it (`this`, vtt, a block's descriptor) are not declared. An explicit object
        // parameter is: Clang marks it an object pointer, not artificial.
        static std::optional<unsigned> sourceIndexOf(const llvm::Function& F,
                                                     const llvm::DILocalVariable& param)
        {
            unsigned artificialBefore = 0;
            for (const llvm::Argument& other : F.args())
            {
                const llvm::DILocalVariable* var = spilledParameter(other);
                if (var && var->getArg() < param.getArg() && var->isArtificial())
                    ++artificialBefore;
            }
            if (artificialBefore >= param.getArg())
                return std::nullopt;
            return param.getArg() - 1 - artificialBefore;
        }
```

In `resolveParameterDebugBinding`, before `finalizeBindingName(binding, Arg);`:

```cpp
        if (spilled && !spilled->isArtificial())
            binding.sourceIndex = sourceIndexOf(F, *spilled);
```

- [ ] **Step 13: Build and run, expect GREEN.** The eight checks pass with their source indexes (recorded on the prototype in `evidence/217/p0/harness/green-unit-prototype.txt`).

- [ ] **Step 14: Format, full suite, warnings.** Run clang-format on the five C++ files, the build (no warning: `-Wpadded` is on), both unit test executables, and the suite. Expected: everything passes, no expectation changed.

- [ ] **Step 15: Commit.**

```bash
git add include/analysis/ParameterDebugBinding.hpp src/analysis/ParameterDebugBinding.cpp test/unit/analyzer_module_unit_tests.cpp
git commit -m "feat(analysis): give a bound parameter its position among the source's"
```

This commit carries its test with its code, so that every commit builds; its failure evidence is the compile error of Step 11.

**Validation of P0.1:**
- `testParameterDebugBinding`: 8 checks pass; both unit test executables pass.
- `virtual-base-vtt.cpp` passes in the default and smt-z3 passes.
- The suite passes on macOS with no expectation changed; no new warning; clang-format clean.

### Task P0.2: measure the effect without the option, then deliver

Expected effect on the rule, without the option:
- base-object constructors (C2, and CI2 for inherited ones) of a class with a virtual base: no suggestion on `vtt`; the parameters after it are reported with their own type, so a suggestion that the shifted type hid can appear, and one it caused disappears;
- constructors that call such a C2: the #167 contract reads the callee's real parameter types; a false "passed where a pointer to const is expected" disappears, and a true one can appear;
- Microsoft ABI targets: no suggestion on `is_most_derived`;
- blocks (`-fblocks`, the default on macOS): no suggestion on `.block_descriptor`;
- nothing else: line, column, confidence and name of every other parameter unchanged; the other rules use `spilledParameter` and `declaredVariable`, which do not change.

- [ ] **Step 1: Compare every fixture's output.**

```bash
D=/Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer-evidence/217/p0/effect
python3 $D/diff_fixture_outputs.py <main checkout> <main binary> <branch binary> $D/fixtures-branch
```

Expected: `changed: 0` among `main`'s fixtures (the script normalizes the logger's process id). The prototype gave `fixtures: 424, changed: 0`.

- [ ] **Step 2: Linux (WSL), suite and corpora.** Build `main` and the branch; run the suite; run both binaries without the option on the self-analysis as CI does (4 chunks, 2 at a time, fast profile, cold caches), zlib `767c4c9`, tinyxml2 `8224e42` and PCRE2 `3e4036e` (without the #215 units). Expected: identical outputs; none of these corpora compiles a class with a virtual base or a block (zlib's `contrib/iostream` is not in its compilation database). The self-analysis time is reported as an indication.

- [ ] **Step 3: Pull request.** After the user's go: push, pull request assigned to `SizzleUnrlsd`, label `bug`, `Fixes #<N>`, body prepared in `evidence/217/p0/pr-body.md` (root cause, effect without the option, measurements). CI; code scanning triaged as `CLAUDE.md` describes, summary in the body. The user merges.

**Validation of P0.2:** no existing fixture output changes; corpora identical without the option; CI green; triage summarized; the effect stated in the body matches the measurements.

### Task I.1: integrate `main` into `217-t-v2`, once P0 is merged

- [ ] **Step 1:** In `wt-217-v2`, `git fetch origin && git merge origin/main`. Conflicts expected in `src/analysis/ConstParamAnalysis.cpp`: keep #221's `issue.debugArgNo` until Task 1.2 removes it.
- [ ] **Step 2:** Build, both unit test executables, suite (T fixtures carry `// analyzer-arg: --experimental-const-suggestion-check`), on macOS and Linux.
- [ ] **Step 3:** Replay `evidence/217/final-review/repro.sh`; record which review cases change because of P0 (expected: `t10` loses its `vtt` suggestion).
- [ ] **Step 4:** Commit the merge (`Merge origin/main into 217-t-v2`).

**Validation of I.1:** every test of #221 passes; only the differences that P0 explains in the review cases.

---

## Stage 1 — flow, identities, plan and scope, facts, correspondences, collisions, trace (§9.3)

Catalogue: D1 (T part), D2, D3, D4, D5, D7, D11, D14, D15, D16. Tests are submitted with their failure evidence at the start of each task.

**Open point for the user (found while planning).** §5.10 makes a stale declaration indeterminate: "Une unité qui déclare l'entité avec l'ancien type sans voir la réécriture rend la suggestion indéterminée". T v1 removes that suggestion, and two checks pin it: `check_const_suggestion_cross_tu` (`("def and a redeclaration", ["def.c", "redecl.c"], [])`) and `check_const_suggestion_compilation_database` (`("a redeclaration, not analyzed", ["def.c", "redecl.c"], "read_value", None)`). These two expectations change with §5.10, but §7 does not list them. Proposed: D17, "redeclaration with the old type in another file", indeterminate, the reason naming the file and the link-edition assumption; both expectations submitted with Task 1.6. To confirm before Task 1.6.

Shared types, in `ConstSuggestionInternal.hpp` (fixed at the stage's start, from §3):
- `enum class Verdict : std::uint8_t { Verified, Refuted, Indeterminate };` and `struct Judgement { Verdict verdict; std::string reason; }`;
- `struct UnitConfiguration { std::string file; UnitCommand command; std::string fingerprint; }`, the fingerprint hashing the normalized arguments and working directory;
- `struct DeclarationSite { std::string file; unsigned offset; }` (real path, offset in the original text);
- `enum class SourceLinkage : std::uint8_t { None, Internal, External };` and `struct EntityIdentity { std::vector<DeclarationSite> redeclarations; SourceLinkage linkage; std::optional<DeclarationSite> templateOrigin; }`;
- `struct Candidate { std::string type; std::string alternative; }` (§3 table);
- `struct SuggestionPlan { EntityIdentity entity; unsigned sourceIndex; Candidate candidate; std::map<std::string, std::vector<Edit>> edits; std::optional<std::string> indeterminate; }`;
- `struct FactKey { std::string unit; std::vector<std::string> instantiationContexts; DeclarationSite spelling; DeclarationSite expansion; }`, `enum class FactKind { Reference, Override, Constant }`, `struct Fact { FactKind kind; FactKey key; std::string value; }`;
- `struct SymbolPair { std::string before; std::string after; }`.

### Task 1.1: units, verdicts, aggregation, report

**Files:** `ConstSuggestionCheck.cpp`, `ConstSuggestionInternal.hpp`, `DiagnosticEmitter.cpp`.
**Interfaces:** produces `Verdict`, `Judgement`, `UnitConfiguration`, `aggregate(const std::vector<Judgement>&) -> Judgement` (§5.10: refuted if a unit refutes; else indeterminate if a unit or a collision is; else verified).
**Work:** one unit per file and configuration that the loader provides; the units that see the change as §5.2 defines them, from T v1's scan; a unit without a command, that cannot be preprocessed or does not compile as it is, unverifiable (judgement step 1); initial compilations cached by unit and entity and shared by every suggestion (§5, step 3); T v1's outcomes mapped onto the three verdicts with T v1's texts; the trace names each unit's file and configuration fingerprint on its own `[const-suggestion]` line.
**Tests:** Review Focus 4, a new `run_test.py` check (a compilation database that lists `cross-tu-const-suggestion-def.c` twice, with and without `-DREAD_TWICE`; the trace names one configuration for that file; the "checked" line stays T v1's).
**Validation:** every T v1 test passes unchanged; the new check passes.

### Task 1.2: the parameter from P0

**Files:** `ConstSuggestionCheck.cpp` (or `ConstSuggestionPlan.cpp` once Task 1.3 creates it), `ConstParamAnalysis.cpp` (#221's `debugArgNo` removed).
**Interfaces:** consumes `ParameterDebugBinding::sourceIndex` (Task P0.1).
**Work:** the parameter is `issue.binding.sourceIndex`; none, or an artificial binding, gives indeterminate (`the parameter is not one the source declares`, `the parameter's position in the source is unknown`).
**Tests:** D1, a new fixture `const-suggestion-virtual-base.cpp` (t10: `x` verified in C1 and C2; at I.1, T rewrites `y` for the suggestion on `x` in C2); Review Focus 2: `const-suggestion-explicit-object.cpp` unchanged.
**Validation:** D1 passes; explicit-object unchanged; every T v1 test passes.

### Task 1.3: planning and plan scope (§5.5)

**Files:** `ConstSuggestionPlan.cpp` (new; T v1's `Planner`, `writtenType`, `expectedType`, `inMacroDefinition` move here), `CMakeLists.txt` (new sources).
**Interfaces:** produces `std::optional<SuggestionPlan> plan(clang::ASTContext&, clang::Preprocessor&, const ConstParamIssue&, llvm::StringRef symbol)`.
**Work:** the entity located by the suggestion's symbol through CodeGen's mangling of each candidate declaration's `GlobalDecl` variants, then described by its source identity; the candidate built on the structured type; edits on each redeclaration's `TypeLoc` under the macro rules of §5.5, macro definitions recognized by the preprocessor (D11); the plan scope (D5).
**Tests:** D5, a new fixture (t3/m.c: a type written in a macro argument that `src` and `dst` share, indeterminate, the suggestion kept); D11, a new `run_test.py` case (t13: `\` followed by spaces, indeterminate); `const-suggestion-macros.c`, `const-suggestion-macro-params.c` and `check_const_suggestion_line_endings` unchanged.
**Validation:** D5 and D11 pass; T v1 macro tests unchanged.

### Task 1.4: compilations, facts R, O and K, scope after the rewrite (§5.3, §5.4, §5.6), judgement steps 1 to 6

**Files:** `ConstSuggestionFacts.cpp` (new; T v1's `Inspector`, `InspectAction`, `compile`, `scan` move here).
**Interfaces:** produces `struct UnitFacts { bool complete; std::vector<Fact> facts; std::map<std::string, llvm::GlobalValue::LinkageTypes> defined; std::vector<DeclarationSite> addressCasts; std::vector<Diagnostic> diagnostics; std::shared_ptr<const ModuleText> code; std::vector<DeclarationSite> entityDeclarations; }` and `UnitFacts inspect(const UnitConfiguration&, const std::map<std::string, std::string>& remapped, const SuggestionPlan&)`.
**Work:** facts keyed by unit, instantiation contexts, spelling and expansion positions mapped back through the plan's edits; scope after the rewrite on source-written declarations only (instantiated signatures go to Task 1.5); an incomplete inspection gives an unknown scope; judgement steps 1 to 6.
**Tests:** D2, a new `run_test.py` check (t1: a member operator used in another file, verified in both files); `const-suggestion-same-code.cpp`, `const-suggestion-override-elsewhere.cpp`, `pick` in `const-suggestion-deduction-refused.cpp`, `generic_selection` in `const-suggestion-must-compile.c` unchanged.
**Validation:** D2 passes; the refutations by R, O and K of T v1's tests unchanged.

### Task 1.5: correspondences and the comparator's pairs (§5.7, §5.9), judgement step 7

**Files:** `ConstSuggestionMatch.cpp` (new), `ConstSuggestionCode.cpp`, `include/analysis/ConstSuggestionCheck.hpp`.
**Interfaces:** produces `std::vector<SymbolPair> correspond(const UnitFacts& before, const UnitFacts& after, const SuggestionPlan&, std::string& missing)`; `codeDifference` gains `const std::vector<SymbolPair>& established`.
**Work:** the entity's variants by `GlobalDecl`; nested entities (lambdas, local statics, local classes, RTTI names); instantiation contexts whose arguments change, paired under the four conditions of §5.7; inherited constructors through `getInheritedConstructor()`, adopted only once shown on t8 and a template case; verified requires every appearing or disappearing symbol to be covered by an established pair; the heuristics only place a difference in the trace.
**Tests:** D7, a new fixture (t8: verified if the technique is shown, otherwise indeterminate, never removed); D14, changed expectation of `call` in `const-suggestion-deduction-refused.cpp` (indeterminate, kept); D16, changed expectation of `signature_size` in `const-suggestion-printed-signature.c` (indeterminate, kept); `const-suggestion-kept*.cpp`, `const-suggestion-abi*.cpp`, `const-suggestion-deduction-kept.cpp`, `print_and_read` unchanged.
**Validation:** D7, D14 and D16 pass; every T v1 "checked" stays verified.

### Task 1.6: collisions, stale declarations, link-edition assumption (§5.10)

**Files:** `ConstSuggestionMatch.cpp`, `README.md`.
**Work:** the linkage table of §5.10; a same entity defined in several units is no collision; a stale declaration gives indeterminate (D17, pending); every reason that depends on linking states the assumption and its scope.
**Tests:** D3, a new `run_test.py` check (t2: C `static` and external homonyms, both directions, verified; the README sentence corrected); D4, a new `run_test.py` check (t9: `f(const int*)` defined in another file, indeterminate with the assumption, files analyzed together and the other file only in the database); Review Focus 3, a new `run_test.py` check (a header defining `static inline` C and `inline` C++ functions, included by two analyzed files, verified); D17, the two changed expectations, if confirmed; the static and anonymous-namespace homonyms of `check_const_suggestion_cross_tu` unchanged.
**Validation:** D3, D4 and Review Focus 3 pass.

### Task 1.7: explicit address casts (D15)

**Files:** `ConstSuggestionFacts.cpp`, `ConstSuggestionCheck.cpp`.
**Work:** a cast of the entity's address is recorded and named; alone, it gives indeterminate.
**Tests:** changed expectations: `("def and an explicit cast", ["def.c", "cast.c"], ["read_value"])` in `check_const_suggestion_cross_tu`, and `("an explicit cast, not analyzed", …)` expecting a "not checked with Clang" reason naming `cross-tu-const-suggestion-cast.c` in `check_const_suggestion_compilation_database`.
**Validation:** both checks pass with their new expectations.

### Task 1.8: trace and README

**Files:** `ConstSuggestionCheck.cpp`, `README.md`.
**Work:** the trace of §4 for each verdict (rule, function and symbol, parameter by name and source index, transformation, unit and configuration, verdict and reason), on `[const-suggestion]` lines after T v1's; the README describes the three verdicts and the limits of §10.
**Tests:** `check_const_suggestion_removal_reason` unchanged; a new check of the trace fields on one verified, one refuted and one indeterminate suggestion.
**Validation:** both pass.

**Validation of stage 1:** the catalogue tests D1 (T), D2, D3, D4, D5, D7, D11, D14, D15, D16 and Review Focus 2 to 4 pass; every T v1 test passes, except the changed expectations of D14, D15, D16 (and D17 if confirmed); suite without and with the option, unit tests, on macOS and Linux; review cases t1 to t15 give the verdicts of §6 for the stage-1 IDs; CI green and code scanning triaged.

## Stage 2 — D6, D8, D10, D12, D13

### Task 2.1: warning policy (D6)
**Work:** `-W<group>` then `-Wno-error=<group>` for each tracked group, in both compilations.
**Tests:** D6, a new fixture (t5) with `// analyzer-arg: --compile-arg=-Werror` and a tracked group disabled, verified; Review Focus 5, a variant with `--compile-arg=-Werror=incompatible-pointer-types-discards-qualifiers` where an introduced warning still refutes.

### Task 2.2: the unit's language (D8)
**Work:** the targeted redeclarations compared with the candidate in the language of each unit.
**Tests:** D8, a new `run_test.py` check (t4: C definition, C++ user through an `extern "C"` header, verified; the C witness unchanged).

### Task 2.3: typedef with a qualifier (D10)
**Work:** a pointer typedef receives the printed candidate, a separately written qualifier is folded in; one written by a macro gives indeterminate.
**Tests:** D10, new fixtures in C and C++ (t3/t.c, t3/t.cpp: verified with `const int *const`).

### Task 2.4: one candidate, displayed (D12, D13)
**Work:** the displayed type and the "checked" line name the candidate: `const T &` for `ReferenceRvaluePreferValue` (passing by value is not checked), `const int *const p` for `PointerConstOnly`.
**Tests:** D12, changed expectations of the two `ReferenceRvaluePreferValue` blocks of `const-suggestion-deduction-kept.cpp`, the only T fixture with that rule; D13, a new fixture (t15: `int* const p` only read, displayed and verified as `const int *const p`). No T v1 fixture covers `PointerConstOnly`, so D13 adds a fixture where §6 expected changed expectations.

**Validation of stage 2:** as stage 1, for D6, D8, D10, D12, D13 and Review Focus 5.

## Stage 3 — measured costs, one Clang invocation

### Task 3.1: measure
**Work:** the `const_suggestion.*` hotspots of `--timing` on the self-analysis with the option, as CI runs it; the per-suggestion copy of each unit's scan, the recompilation of a failing initial compilation, the duplicated construction of the invocation, each measured.
**Validation:** a measurement report in `evidence/217/v2-cost/`; only measurable costs go to Task 3.2.

### Task 3.2: remove the measured costs; build the invocation once
**Work:** shared scans instead of copies; a failing initial compilation cached as failing; one function builds the Clang invocation for scanning and compiling, reusing the existing path normalization.
**Validation:** outputs identical before and after the stage (suite, review cases, corpora with the option); the gain measured.

## Final phase

### Task F.1: corpora
**Work:** on Linux, `src/`, zlib, tinyxml2 and PCRE2, three outputs: without the option, T v1, T v2. Each suggestion classified verified, kept indeterminate, or removed on an established refutation. Each disappearance against the output without the option is explained by its violation; each difference with T v1 is explained.

### Task F.2: cost
**Work:** the protocol of §8. The criterion is +10 % on the self-analysis.

### Task F.3: delivery
**Work:** README; a draft pull request after the user's go; CI; code scanning triaged; final review on the updated base. No merge.

**Validation of the final phase:** every classification explained; cost reported against the criterion; CI green; triage summarized.
