# Const Suggestion Check v2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix `ParameterDebugBinding` on `main` (P0), then rework T, the experimental Clang check of `ConstParameterNotModified` suggestions, so that each verdict is justified and tied to the exact candidate, units and configurations it checked.

**Architecture:** P0 binds each IR argument to its own debug declaration: its type, whether the source declares it, and its position among the source parameters. T v2 then runs a fixed flow on `217-t-v2`: initial compilation and planning in the defining file, initial and modified compilations of each unit that sees the change, correspondences established from the AST, a seven-step judgement per unit, aggregation, report. Verdicts are verified, refuted or indeterminate; the IR comparator only gains the established pairs as an input.

**Tech Stack:** C++20, LLVM and Clang 20 (IR, AST, CodeGen `GlobalDecl`), `run_test.py` fixtures and checks, `stack_usage_analyzer_unit_tests`.

**Spec:** `docs/superpowers/specs/2026-10-16-const-suggestion-check-v2-design.md`, approved at `8b283a8`.

**Level of detail.** P0 is planned step by step with its code, validated in a throwaway prototype (evidence below). Stages 1 to 3 and the final phase are planned to the level of tasks, files, interfaces, tests and validation criteria. The step-level code of a stage is added to this plan when that stage starts: P0 and the updated `main` are integrated first, and they change the code these stages modify. A stage starts only once the previous one is validated. Stage 1's detail was written at its start, once Task I.1 was validated.

**Evidence:** `coretrace-stack-analyzer-evidence/217/p0/` (`repro/`, `harness/`, `fixture/`, `effect/`, `prototype-v1.diff`, `prototype-v2.diff`, `prototype-v3.diff`, `ir/`, `variadic/`, `review-v3/`).

**Execution:** native (superpowers:executing-plans), as the user chose; an independent review of P0 before its merge (Task P0.2). Progress ledger: `wt-217-v2/.superpowers/sdd/2026-10-16-const-suggestion-check-v2/progress.md`.

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

1. **Windows targets (Microsoft C++ ABI).** A constructor of a class with a virtual base takes `is_most_derived` after the source parameters, or second when it is variadic; `main` types it as the last source parameter and reports it (`double *is_most_derived`). It must be artificial, and the other parameters keep their types. Pinned in Task P0.1 (unit test, `--target=x86_64-pc-windows-msvc`).
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
| `src/analysis/ParameterDebugBinding.cpp` | Type and artificiality from the parameter's own declaration; the hidden return argument recognized by its attribute; source index. |
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

Root cause, at `main` `d1d9271`. At `-O0`, `buildDebugParamLayout` indexes the function's `DISubroutineType` with the debug number (`arg:`) of the argument's spilled declaration. That number counts every parameter; the subroutine type lists `this` and a block's descriptor, but not what the C++ ABI adds: the `vtt` of a base-object constructor (C2) of a class with a virtual base, after `this`, or `is_most_derived` under the Microsoft ABI, last, or second in a variadic constructor. Artificiality is read from the entry found there, not from the parameter's own declaration. Hence:
- `vtt` gets the type of the first source parameter, and is reported (`int *vtt`);
- the parameters after it get the type of the next one (`double *x` for `int* x`);
- a constructor that calls such a C2 trusts the shifted type as a `const` contract (#167): `Leaf::Leaf(int* p, …) : Writer(p, …)` reports `p` as "passed where a pointer to const is expected" although `Writer` writes through it;
- `is_most_derived` (an `int`) is reported as `double *is_most_derived`;
- a block's `.block_descriptor` is reported as `<unknown type> *.block_descriptor`.

The fix takes each spilled parameter's type and artificiality from its own declaration, the one `spilledParameter` finds; this alone binds every case above. The subroutine type stays a fallback for a declaration that gives no type, only where its entries are established, and for arguments without a declaration, the hidden return argument being recognized by its sret attribute wherever the ABI puts it. A first version left the null entry that ends a variadic function's type out of the positional count instead: the independent review showed it misbound optimized variadic functions, and it was reverted. A first prototype reconciled positions in the subroutine type instead (`evidence/217/p0/prototype-v2.diff`); it was dropped once the direct declaration was shown to bind every case, with no output change on the 424 fixtures.

Reproductions: `evidence/217/p0/repro/` (`main-d1d9271.txt`, `caller-main-d1d9271.txt`, `block-main-d1d9271.txt`).

### Task P0.1: bind each parameter to its own declaration

**Files:**
- Create: `test/unit/parameter_binding_input.cpp`, `test/unit/parameter_binding_block_input.c`, `test/unit/parameter_binding_unknown_input.ll` (Step 12)
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

- [ ] **Step 7: Add the Clang cases the user authorized, one commit each.** A variadic function's debug type ends with a null entry for `...`; an empty parameter takes no IR argument but keeps its debug declaration; a variadic function can return a struct through a hidden argument.

Step 7a, variadic constructors. Append to `test/unit/parameter_binding_input.cpp`:

```cpp
// A variadic constructor: the debug type of the function ends with a null entry for `...`, which
// no parameter takes. Under the Microsoft ABI, is_most_derived comes second.
struct Varargs : virtual Base
{
    Varargs(int* x, double* y, ...);
};

Varargs::Varargs(int* x, double* y, ...)
{
    seed = *x + static_cast<int>(*y);
}

```

In `testParameterDebugBinding`, give `describe` a branch for a parameter without a type, between the artificial branch and the pointer one:

```cpp
                else if (!binding.type)
                {
                    text += "no type";
                }
```

and add, after the block's check:

```cpp
        // A variadic function's debug type ends with a null entry for `...`, which no parameter
        // takes.
        check(itanium, "_ZN7VarargsC2EPiPdz",
              {"this: artificial", "vtt: artificial", "x: int *", "y: double *"});
        check(microsoft, "??0Varargs@@QEAA@PEAHPEANZZ",
              {"this: artificial", "is_most_derived: artificial", "x: int *", "y: double *"});
```

Expected RED, besides the four failures of Step 5:

```
[FAIL] ParameterDebugBinding: _ZN7VarargsC2EPiPdz [this: artificial] [vtt: int *] [x: double *] [y: no type]
[FAIL] ParameterDebugBinding: ??0Varargs@@QEAA@PEAHPEANZZ [this: artificial] [is_most_derived: int *] [x: double *] [y: no type]
```

Commit: `test(const-param): a variadic constructor keeps the types of its parameters` (the input and the test file).

Step 7b, an empty parameter. Append to the input:

```cpp
// An empty parameter takes no IR argument, but keeps its debug declaration and its number.
struct Tag
{
};

struct Tagged : virtual Base
{
    Tagged(int* x, Tag tag);
};

Tagged::Tagged(int* x, Tag tag)
{
    (void)tag;
    seed = *x;
}

```

and, after the variadic checks: `check(itanium, "_ZN6TaggedC2EPi3Tag", {"this: artificial", "vtt: artificial", "x: int *"});`; the comment above the variadic checks gains `; an empty parameter takes no IR argument, but keeps its debug declaration`. Expected RED: `[FAIL] ParameterDebugBinding: _ZN6TaggedC2EPi3Tag [this: artificial] [vtt: int *] [x: ? *]`. Commit: `test(const-param): an empty parameter shifts no other parameter`.

Step 7c, a variadic hidden return. Append to the input:

```cpp
// A variadic function that returns a struct through a hidden argument.
struct Big
{
    long values[8];
};

Big make_big(int* p, ...)
{
    Big big{};
    big.values[0] = *p;
    return big;
}
```

and add `check(itanium, "_Z8make_bigPiz", {"agg.result: artificial", "p: int *"});`. Expected RED: `[FAIL] ParameterDebugBinding: _Z8make_bigPiz [agg.result: int *] [p: int *]`. Commit: `test(const-param): a variadic function's hidden return argument is artificial`.

Step 7d, the fixture's variadic constructor. The fixture becomes:

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

struct Variadic : virtual Base
{
    Variadic(int* v, double* w, ...);
};

// x is only read: both variants, C1 and C2, report it with its own type.
// not contains: current type: double *x
// at line 28, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'Reader::Reader(int*, double*)' is never used to modify the pointed object
//          ↳ current type: int *x
//          ↳ suggested type: const int *x
Reader::Reader(int* x, double* y)
{
    seed = *x;
    *y = 0;
}

// A variadic constructor: the `...` that ends the debug type is no parameter's.
// not contains: current type: double *v
// at line 40, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'v' in function 'Variadic::Variadic(int*, double*, ...)' is never used to modify the pointed object
//          ↳ current type: int *v
//          ↳ suggested type: const int *v
Variadic::Variadic(int* v, double* w, ...)
{
    seed = *v;
    *w = 0;
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

Expected RED: the fixture also finds `current type: double *v`, in both passes. Commit: `test(const-param): report a variadic constructor's parameter with its own type`.

- [ ] **Step 8: Take a parameter's type and its artificiality from its own declaration.** In `resolveParameterDebugBinding`, replace `if (layout.isHiddenAbiArg)` with:

```cpp
        // The declaration of the spilled parameter says whether the source declares it.
        const llvm::DILocalVariable* spilled = spilledParameter(Arg);
        if (layout.isHiddenAbiArg || isSyntheticDebugVariable(spilled))
```

and, before the fallback on the subroutine type (`if (!binding.type && layout.hasCanonicalDebugIndex && layout.subroutineType)`):

```cpp
        // The spilled parameter's own declaration gives its type first.
        if (!binding.type && spilled)
            binding.type = spilled->getType();
```

Expected: every check passes but `_Z8make_bigPiz`, whose hidden argument is not spilled; the fixture passes in both passes (14 of 14). Commit: `fix(const-param): take a parameter's type and artificiality from its own declaration`.

- [ ] **Step 9: Leave the hidden return argument out of the positions.** In `buildDebugParamLayout`, before `const unsigned hiddenPrefix = irArgCount - debugParamCount;`, replace the computation of `irArgCount` and `debugParamCount` with:

```cpp
            // The hidden return argument is no source parameter, wherever the ABI puts it: first
            // under the Itanium ABI, after `this` under the Microsoft one. Positions leave it out.
            const llvm::Argument* hiddenReturn = nullptr;
            for (const llvm::Argument& other : F.args())
            {
                if (other.hasStructRetAttr())
                    hiddenReturn = &other;
            }
            if (hiddenReturn == &Arg)
            {
                layout.isHiddenAbiArg = true;
                return layout;
            }
            const unsigned irArgCount =
                static_cast<unsigned>(F.arg_size()) - (hiddenReturn ? 1 : 0);
            const unsigned argNo =
                Arg.getArgNo() -
                (hiddenReturn && hiddenReturn->getArgNo() < Arg.getArgNo() ? 1 : 0);

            const unsigned debugParamCount = static_cast<unsigned>(typeArray.size() - 1);
            if (irArgCount < debugParamCount)
                return layout;

```

and use `argNo` instead of `Arg.getArgNo()` in the two lines that follow (`if (argNo < hiddenPrefix)`, `const unsigned debugParamOffset = argNo - hiddenPrefix;`). Expected: the 12 checks pass. Commit: `fix(const-param): leave a struct return argument out of the parameters' positions`.

Executed history: a first Step 9 left the `...` entry out of the positional count (522344e); the independent review showed that optimized variadic functions were then misbound, so 21cfb67 reverts it, with `_Z8make_bigPiz` failing again, and 7cf76bf is this step.

- [ ] **Step 10: Expect source indexes in the unit test.** In `describe`, after the `if`/`else` chain, add:

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
        // A variadic function's debug type ends with a null entry for `...`, which no parameter
        // takes; an empty parameter takes no IR argument, but keeps its debug declaration.
        check(itanium, "_ZN7VarargsC2EPiPdz",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(microsoft, "??0Varargs@@QEAA@PEAHPEANZZ",
              {"this: artificial, source none", "is_most_derived: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZN6TaggedC2EPi3Tag",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0"});
        check(itanium, "_Z8make_bigPiz",
              {"agg.result: artificial, source none", "p: int *, source 0"});
```

and the comment above the test with:

```cpp
    /// resolveParameterDebugBinding: each parameter gets its own type, and its position among the
    /// parameters the source declares. The others are artificial: `this`, the vtt that the
    /// base-object constructor (C2) of a class with a virtual base takes, is_most_derived under the
    /// Microsoft ABI, a block's descriptor.
```

`self` stays artificial, as on `main`: Clang marks it an object pointer, which the binding has always treated as synthetic. Its source index is 0 all the same, since the source declares it. `tag` takes no IR argument, so no check lists it. Expected RED: `error: no member named 'sourceIndex' in 'ctrace::stack::analysis::ParameterDebugBinding'`.

- [ ] **Step 11: Give each parameter its source index.** In `include/analysis/ParameterDebugBinding.hpp`, add `#include <optional>` after `#include <cstdint>`, and after `unsigned column = 0;`:

```cpp
        // The parameter's position among those the source declares, from 0. None for one the
        // source does not declare (`this`, vtt, a block's descriptor), or that -O0 code does not
        // spill.
        std::optional<unsigned> sourceIndex;
```

The struct keeps no implicit padding: `std::optional<unsigned>` takes 8 bytes at a 4-byte boundary, after `column`. In `src/analysis/ParameterDebugBinding.cpp`, add `#include <optional>` after `#include <limits>`, and after `isSyntheticDebugVariable`:

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

Expected: the 12 checks pass with their source indexes. Commit: `feat(analysis): give a bound parameter its position among the source's`; it carries its test with its code, so that every commit builds.

- [ ] **Step 12: Untyped declarations, once the user authorizes these cases.** A declaration that gives no type does not occur in Clang's output; the subroutine type then serves only where its entries are established, which the hand-written IR below checks. Create `test/unit/parameter_binding_unknown_input.ll`:

```llvm
; SPDX-License-Identifier: Apache-2.0
; A parameter's declaration gives its type; when it gives none, the debug type of the function
; serves only where its entries are the declared parameters in order: when no artificial parameter
; other than `this` exists. vtt, is_most_derived or a block's descriptor make the position
; ambiguous, since the debug type lists some of them and not others.

; An artificial parameter other than `this`: the debug type's entries are ambiguous. x's
; declaration gives its type, double *, which is kept.
define void @ambiguous_typed(ptr %hidden, ptr %x) !dbg !20 {
entry:
  %hidden.addr = alloca ptr
  %x.addr = alloca ptr
  store ptr %hidden, ptr %hidden.addr
    #dbg_declare(ptr %hidden.addr, !21, !DIExpression(), !23)
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !22, !DIExpression(), !23)
  ret void, !dbg !23
}

; The same, but x's declaration gives no type: x stays without one.
define void @ambiguous_untyped(ptr %hidden, ptr %x) !dbg !30 {
entry:
  %hidden.addr = alloca ptr
  %x.addr = alloca ptr
  store ptr %hidden, ptr %hidden.addr
    #dbg_declare(ptr %hidden.addr, !31, !DIExpression(), !33)
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !32, !DIExpression(), !33)
  ret void, !dbg !33
}

; No artificial parameter: the debug type's entries are the parameters, in order. x's declaration
; gives no type, the debug type gives double *.
define void @established_untyped(ptr %x) !dbg !40 {
entry:
  %x.addr = alloca ptr
  store ptr %x, ptr %x.addr
    #dbg_declare(ptr %x.addr, !41, !DIExpression(), !42)
  ret void, !dbg !42
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus_14, file: !1, producer: "hand-written", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "parameter_binding_unknown_input.ll", directory: ".")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!10 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!11 = !DIBasicType(name: "double", size: 64, encoding: DW_ATE_float)
!12 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64)
!13 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !11, size: 64)
!14 = !DISubroutineType(types: !15)
!15 = !{null, !12, !12, !12}
!16 = !DISubroutineType(types: !17)
!17 = !{null, !13}
!20 = distinct !DISubprogram(name: "ambiguous_typed", scope: !1, file: !1, line: 1, type: !14, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!21 = !DILocalVariable(name: "hidden", arg: 1, scope: !20, file: !1, line: 1, type: !12, flags: DIFlagArtificial)
!22 = !DILocalVariable(name: "x", arg: 2, scope: !20, file: !1, line: 1, type: !13)
!23 = !DILocation(line: 1, scope: !20)
!30 = distinct !DISubprogram(name: "ambiguous_untyped", scope: !1, file: !1, line: 2, type: !14, scopeLine: 2, spFlags: DISPFlagDefinition, unit: !0)
!31 = !DILocalVariable(name: "hidden", arg: 1, scope: !30, file: !1, line: 2, type: !12, flags: DIFlagArtificial)
!32 = !DILocalVariable(name: "x", arg: 2, scope: !30, file: !1, line: 2)
!33 = !DILocation(line: 2, scope: !30)
!40 = distinct !DISubprogram(name: "established_untyped", scope: !1, file: !1, line: 3, type: !16, scopeLine: 3, spFlags: DISPFlagDefinition, unit: !0)
!41 = !DILocalVariable(name: "x", arg: 1, scope: !40, file: !1, line: 3)
!42 = !DILocation(line: 3, scope: !40)
```

Declare `LoadedModule unestablished;` after `LoadedModule blocks;`, load it last:

```cpp
            !load("parameter_binding_block_input.c", {"-fblocks"}, blocks) ||
            !load("parameter_binding_unknown_input.ll", {}, unestablished))
            return false;
```

and add, after the last check of Step 10:

```cpp
        // A declaration's type is kept; without one, the debug type serves only where its
        // entries are established.
        check(unestablished, "ambiguous_typed",
              {"hidden: artificial, source none", "x: double *, source 0"});
        check(unestablished, "ambiguous_untyped",
              {"hidden: artificial, source none", "x: no type, source 0"});
        check(unestablished, "established_untyped", {"x: double *, source 0"});

```

Expected RED: `[FAIL] ParameterDebugBinding: ambiguous_untyped [hidden: artificial, source none] [x: int *, source 0]`; `ambiguous_typed` passes since Step 8, `established_untyped` keeps `main`'s fallback. Commit: `test(const-param): an untyped parameter takes the debug type only where established`.

- [ ] **Step 13: Use the debug type of the function only where its entries are established.** Add `#include <llvm/ADT/STLExtras.h>` before `#include <llvm/ADT/SmallVector.h>`, and before `buildDebugParamLayout`:

```cpp
        // Whether F has an artificial parameter other than `this`: a vtt, an is_most_derived or a
        // block's descriptor. The subroutine type lists the descriptor, not what the C++ ABI adds.
        static bool hasArtificialOtherThanThis(const llvm::Function& F)
        {
            return llvm::any_of(F.args(),
                                [](const llvm::Argument& other)
                                {
                                    const llvm::DILocalVariable* var = spilledParameter(other);
                                    return var && var->isArtificial() && !var->isObjectPointer();
                                });
        }

```

In `buildDebugParamLayout`, replace the spilled branch with:

```cpp
            // The spill slot names the source parameter whatever the ABI did to the list; the
            // position below only serves arguments that are not spilled, like a hidden sret.
            if (const llvm::DILocalVariable* spilled = spilledParameter(Arg);
                spilled && spilled->getArg() != 0)
            {
                // Its declaration gives its type. Without one, the subroutine type's entry at its
                // debug number is its own only when no parameter makes that position ambiguous.
                if (spilled->getType() ||
                    (spilled->getArg() <= listed && !hasArtificialOtherThanThis(F)))
                {
                    layout.debugArgIndex = spilled->getArg();
                    layout.hasCanonicalDebugIndex = true;
                }
                return layout;
            }

```

Expected: the 15 checks pass. Mutants checked on the prototype: without the fallback for an untyped declaration, `established_untyped` fails; with a fallback even where the position is ambiguous, `ambiguous_untyped` fails. Commit: `fix(const-param): use the debug type only where its entries are established`.

- [ ] **Step 14: Cases from the independent review, once the user authorizes them; one commit each, subjects of at most 84 characters (`scripts/ci/commit_checker.py`).** In `test/unit/parameter_binding_input.cpp`, a method of `Maker` returning `Big` (under the Microsoft ABI its hidden return argument comes after `this`) and `TagFirst(Tag tag, int* x)` (an empty first parameter); `test/unit/parameter_binding_o2_input.ll`, generated by Clang 20 at `-O2` for x86-64 Linux from a variadic constructor of a class with a virtual base and a variadic function taking a struct by value; `describe` names a type that is not a pointer; checks: `_ZN8TagFirstC2E3TagPi` (`x: int *, source 1`), `_ZN8VariadicC2EPiPdz` and `split_variadic` at `-O2` (each argument bound to its own declaration), and the Microsoft hidden return argument without a type. A fixture, `implicit-move-constructor.cpp`: an implicitly defined move constructor's parameter is not reported. Code and failure evidence: `evidence/217/p0/proposals/` and `harness/test_G.inc`.

- [ ] **Step 15: Format, full suite, warnings.** Run clang-format on the changed C++ files, the build (no warning: `-Wpadded` is on), both unit test executables, and the suite. Expected: everything passes, no expectation changed; a formatting change, if any, goes in its own commit.

**Validation of P0.1:**
- `testParameterDebugBinding`: every check passes (12 before Step 12, 15 after Step 13, 19 after Step 14); both unit test executables pass.
- `virtual-base-vtt.cpp` passes in the default and smt-z3 passes (14 of 14).
- The suite passes on macOS with no expectation changed; no new warning; clang-format clean.

### Task P0.2: measure the effect without the option, then deliver

Expected effect on the rule, without the option:
- base-object constructors (C2, and CI2 for inherited ones) of a class with a virtual base: no suggestion on `vtt`; the parameters after it are reported with their own type, so a suggestion that the shifted type hid can appear, and one it caused disappears;
- constructors that call such a C2: the #167 contract reads the callee's real parameter types; a false "passed where a pointer to const is expected" disappears, and a true one can appear;
- Microsoft ABI targets: no suggestion on `is_most_derived`;
- blocks (`-fblocks`, the default on macOS): no suggestion on `.block_descriptor`;
- variadic functions: the constructors above keep their types when variadic, and a hidden return argument is artificial wherever the ABI puts it; under the Microsoft ABI, the one after `this` no longer carries `this`'s const contract;
- parameters that Clang marks artificial are no longer reported: the parameter of an implicitly defined special member (an implicit move constructor's), the hidden argument that `pass_object_size` adds, the `__context` of a captured statement;
- a parameter whose declaration gives no type, in a function with an artificial parameter other than `this`, gets no type instead of a guessed one, so the rule does not report it (Clang always gives the type);
- nothing else: line, column, confidence and name of every other parameter unchanged; the other rules use `spilledParameter` and `declaredVariable`, which do not change.

- [ ] **Step 1: Compare every fixture's output.**

```bash
D=/Users/hugopayet/Desktop/CLaude/coretrace-stack-analyzer-evidence/217/p0/effect
python3 $D/diff_fixture_outputs.py <main checkout> <main binary> <branch binary> $D/fixtures-branch
```

Expected: `changed: 0` among `main`'s fixtures (the script normalizes the logger's process id). The prototype gave `fixtures: 424, changed: 0`.

- [ ] **Step 2: Linux (WSL), suite and corpora.** Build `main` and the branch; run the suite; run both binaries without the option on the self-analysis as CI does (4 chunks, 2 at a time, fast profile, cold caches), zlib `767c4c9`, tinyxml2 `8224e42` and PCRE2 `3e4036e` (without the #215 units). Expected: identical outputs; none of these corpora compiles a class with a virtual base or a block (zlib's `contrib/iostream` is not in its compilation database). The self-analysis time is reported as an indication.

- [ ] **Step 3: Independent review.** A fresh reviewer on the most capable model reviews the P0 branch against this plan and the spec (`review-package`, then `requesting-code-review`'s `code-reviewer.md`); its findings are sorted and fixed as superpowers:executing-plans says, each fix with a test that failed first, and reported to the user before the merge.

- [ ] **Step 4: Pull request.** After the user's go: push, pull request assigned to `SizzleUnrlsd`, label `bug`, `Fixes #<N>`, body prepared in `evidence/217/p0/pr-body.md` (root cause, effect without the option, measurements). CI; code scanning triaged as `CLAUDE.md` describes, summary in the body. The user merges.

**Validation of P0.2:** no existing fixture output changes; corpora identical without the option; independent review done and its findings handled; CI green; triage summarized; the effect stated in the body matches the measurements.

### Task I.1: integrate `main` into `217-t-v2`, once P0 is merged

- [ ] **Step 1:** In `wt-217-v2`, `git fetch origin && git merge origin/main`. Conflicts expected in `src/analysis/ConstParamAnalysis.cpp`: keep #221's `issue.debugArgNo` until Task 1.2 removes it.
- [ ] **Step 2:** Build, both unit test executables, suite (T fixtures carry `// analyzer-arg: --experimental-const-suggestion-check`), on macOS and Linux.
- [ ] **Step 3:** Replay `evidence/217/final-review/repro.sh`; record which review cases change because of P0 (expected: `t10` loses its `vtt` suggestion).
- [ ] **Step 4:** Commit the merge (`Merge origin/main into 217-t-v2`).

**Validation of I.1:** every test of #221 passes; only the differences that P0 explains in the review cases.

---

## Stage 1 — flow, identities, plan and scope, facts, correspondences, collisions, trace (§9.3)

Catalogue: D1 (T part), D2, D3, D4, D5, D7, D11, D14, D15, D16, D17.

**Detail, written at the stage's start** (Task I.1 validated at `64ed37e`). Each new or changed test is written out in full with its failure at `64ed37e`. They were run from `evidence/217/stage1/tests/`: the proposed `run_test.py` (`patched/`), the fixtures (`fixtures/new/`, `fixtures/changed/`), the runner `run_red.py` and its output `red-64ed37e.txt`. A test enters the repository only once the user authorizes it; its task starts by running it again to watch it fail. For the code, this section gives the interfaces, the decision logic and every new text; T v1's code that only moves keeps its form and is named.

Commands used by the steps, from the worktree root:
- one check: `python3 -c 'import sys, run_test as r; r.RUN_CONFIG.cache_enabled = False; sys.exit(not r.<check>())'`;
- one fixture: `python3 -c 'import sys, run_test as r; from pathlib import Path; r.RUN_CONFIG.cache_enabled = False; ok, total, passed, report = r.check_file(Path("test/pointer_reference-const_correctness/<fixture>")); print(report); sys.exit(not ok)'`;
- build, unit tests, suite and format: Global Constraints.

**D17 (confirmed by the user).** §5.10 makes a stale declaration indeterminate. T v1 removes that suggestion, and two checks pin it: `check_const_suggestion_cross_tu` (`("def and a redeclaration", ["def.c", "redecl.c"], [])`) and `check_const_suggestion_compilation_database` (`("a redeclaration, not analyzed", ["def.c", "redecl.c"], "read_value", None)`). Both expectations change in Task 1.6, submitted with their failure evidence; the spec's catalogue and §7 list D17. Indeterminate keeps the suggestion as not checked: it does not show that the suggestion is valid, and the reason says so through the file and the link-edition assumption.

### Texts

Every new text, verbatim. `<function>` is the name the diagnostic shows, `<symbol>` a symbol, `<file>` a real path, `'<a>'` a demangled name.

Reasons of an indeterminate verdict, after `not checked with Clang: ` (shown in the diagnostic):

| Reason | Task | When |
|---|---|---|
| `the parameter's type is written in a macro argument that writes other parameters too` | 1.3 | D5: the text an edit rewrites writes another parameter |
| `the parameter's type is written in a macro argument that writes other declarations too` | 1.3 | the same, for another declaration |
| `<file> could not be inspected once rewritten` | 1.4 | step 2 |
| `the rewrite also changes the type of <name> (<file>:<line>)` | 1.4 | step 3 |
| `the generated code changes, and no reference, override or constant condition explains it: <difference>` | 1.5 | step 7 (D16); `<difference>` is the comparator's text |
| `'<a>' and '<b>' both become '<c>': their correspondence is not one to one` | 1.5 | §5.7, D14 |
| `'<a>' becomes both '<b>' and '<c>': their correspondence is not one to one` | 1.5 | §5.7 |
| `<symbol> is also defined in <file>; if <defining file> and <file> are linked together, the two definitions collide` | 1.6 | §5.10, strong (D4) |
| `<symbol> is also defined, weak, in <file>; if <defining file> and <file> are linked together, the definition in <file> is used and the other one is ignored` | 1.6 | §5.10, weak |
| `<symbol> is also defined in <file>, as an inline or template definition; if <defining file> and <file> are linked together, only one of the two is kept, and the check does not establish that they are equivalent` | 1.6 | §5.10, `weak_odr`, `linkonce_odr` |
| `the rewrite gives <function> the symbol of another declaration (<file>:<line>); if a file linked with <defining file> defines it, the two definitions collide` | 1.6 | ruling 3 |
| `<file> declares <function> with its current type and does not see the rewrite: if <defining file> and <file> are linked together, that declaration no longer matches the rewritten definition` | 1.6 | D17 |
| `<file>:<line> converts the address of <function> with an explicit cast` | 1.7 | D15 |

Reasons of a refuted verdict, in the trace only (a refuted suggestion is removed), after T v1's `removed, `:

| Reason | Task |
|---|---|
| `<file>:<line> refers to '<b>' instead of '<a>'` | 1.4, R |
| `'<method>' overrides <list> instead of <list>` (`'<a>', '<b>'`, or `nothing`) | 1.4, O |
| `the condition at <file>:<line> becomes <true or false>` | 1.4, K (`if constexpr`) |
| `the _Generic selection at <file>:<line> chooses another association` | 1.4, K |

T v1's texts stay, `does not compile: <file>:<line>: …` and `gains a checked warning: <file>:<line>: …` included.

Trace (`--verbose`). T v1's line `[const-suggestion] <symbol> parameter <name>: <outcome>` comes first, unchanged. The new lines follow it; none matches T v1's pattern `(\S+) parameter (\S+): `, which `check_const_suggestion_removal_reason` reads:

```
[const-suggestion] <symbol> parameter <name>, suggestion: <rule id> on <function>, source index <i>
[const-suggestion] <symbol> parameter <name>, transformation: <current type> becomes <candidate> (<alternative>)
[const-suggestion] <symbol> parameter <name>, rewritten at: <file>:<line>
[const-suggestion] <symbol> parameter <name>, unit <file> [<fingerprint>]: <verdict>[: <reason>]
[const-suggestion] <symbol> parameter <name>, verdict: <verdict>[: <reason>]
```
- one `rewritten at` line per edit, none when the plan is not executed; one `unit` line per unit judged, in judgement order (Task 1.1 adds these lines, Task 1.8 the others);
- `<verdict>`: `verified`, `refuted` or `indeterminate`;
- `<alternative>`, from §3: `pointer to const`, `pointer to const, top-level const kept`, `reference to const`, `reference to const; passing by value is not checked`;
- `<fingerprint>`: 16 lowercase hexadecimal digits, `llvm::xxh3_64bits` of the unit's arguments and working directory as T builds them, each followed by a NUL byte.

### Rulings

The spec leaves these open; each one is the user's to overturn.
1. **Which reason is shown.** A refuted suggestion gives the reason of the first unit that refutes it, in judgement order: the defining unit, then the others in T v1's order. An indeterminate one gives the reason of the first indeterminate unit in that order; if no unit is indeterminate, the first collision's, a definition before a declaration. Cost if wrong: another reason is shown; the verdict stays.
2. **The initial compilation of another unit is shared by the suggestions of one module.** T runs once per analyzed module (`checkConstSuggestions`): a unit that sees the changes of two modules is compiled once as it is for each. Sharing it across modules needs the facts of entities not planned yet; Task 3.1 measures what it would save. Cost if wrong: one more initial compilation per module and unit.
3. **A declaration of another entity that the rewrite merges into the entity** (t9 without `b.cpp`: `api.hpp` also declares `f(const int*)`) is a possible collision (§3, "collision possible entre fichiers"): indeterminate, with its own reason, shown after a definition's. Cost if wrong: a suggestion kept as not checked that a definition elsewhere would refute or not.
4. **R identifies a target by its declarations.** The value of an R fact is its target's declaration sites, mapped back to the original text; for a template specialization or an implicit function, which have no declaration of their own, its symbol, mapped back through the established pairs. An R fact is unchanged when its target after the rewrite keeps every declaration it had: a merge (ruling 3) adds the entity's, and the collisions judge it, not the facts. A target that loses a declaration, or another target, refutes. A symbol alone would not do: once t9 is rewritten, `_Z1fPKi` names both the entity and the overload it merges with. Cost if wrong: a merge refuted, or a reference to the wrong declaration missed.
5. **D3 is pinned in Task 1.4, not 1.6**: both directions are fixed by the source identity with its linkage, which Task 1.4 brings. Task 1.6 keeps D4, Review Focus 3 and D17.
6. **Task 1.1 gets a second test**, the shared initial compilation, counted with `--timing`: the plan named it as work but did not pin it.
7. **D7's fixture holds four cases**: t8, and as templates a base class template, a derived class template and a constructor template. The technique of §5.7 holds on all four in `evidence/217/stage1/d7-probe/` (`probe.cpp`, `probe-output.txt`): `getInheritedConstructor()` finds the constructor each inheriting constructor comes from, and the symbols CodeGen gives the inheriting constructors are the IR's, before and after. So D7 expects verified.
8. **The two reasons of Task 1.2 are not added.** After P0, the rule reports no artificial parameter, and every parameter of the `-O0` code the analyzer compiles from sources is spilled, so it has a source index: no input reaches those reasons, and no test could fail first. A binding without a source index, or artificial, keeps T v1's text `the parameter is not found in the source`. Cost if wrong: a less precise reason on an input not found.
9. **The comparator's name heuristics are removed** once the established pairs replace them (Task 1.5): the masked encodings, `withoutConst`, `variantOf` and the template keys. The difference text keeps listing the symbols left without a pair, which locates the difference, as §5.9 asks. Cost if wrong: a trace that no longer suggests a probable counterpart.
10. **D12 (Task 2.4) also changes the `ReferenceRvaluePreferValue` block that D14 adds** to `const-suggestion-deduction-refused.cpp`.
11. **Task 1.4 gets a check of the scope after the rewrite** (`check_const_suggestion_header_read_differently`): a header that another file reads as other declarations, through a macro it defines otherwise. Without it, step 3 of §5.8 would have no failing test: the plan's scope (Task 1.3) holds in the defining unit.
12. **No error limit, and step 2 without a test.** T's compilations set no error limit. Otherwise a rewrite that breaks more than 19 uses would end in "too many errors", which is fatal, and step 2 would keep as not checked a suggestion that T v1 removes. Step 2 then guards only a fatal error that the rewrite itself brings, which inserting `const` does not bring in any input of the suite: it has no failing test. Cost if wrong: an untested guard.
13. **Task 1.4 gets a fixture where R and K refute without a change of code** (`const-suggestion-facts-without-code-change.cpp`). T v1 compares only code and keeps both suggestions as checked; since every refutation of T v1's tests comes with a code difference, the facts of Task 1.4 would otherwise have no failing test.

### Shared types

In `ConstSuggestionInternal.hpp`, each added by the task that first needs it:
```cpp
enum class Verdict : std::uint8_t { Verified, Refuted, Indeterminate };              // 1.1
struct Judgement { Verdict verdict = Verdict::Verified; std::string reason; };      // 1.1
struct UnitConfiguration { std::string file; UnitCommand command; std::string fingerprint; }; // 1.1
struct DeclarationSite                                                                // 1.4
{
    std::string file;    // real path
    unsigned offset = 0; // in the original text
    auto operator<=>(const DeclarationSite&) const = default;
};
enum class SourceLinkage : std::uint8_t { None, Internal, External };               // 1.4
struct EntityIdentity                                                                 // 1.4
{
    std::vector<DeclarationSite> redeclarations; // the defining unit's, sorted
    SourceLinkage linkage = SourceLinkage::None;
    std::optional<DeclarationSite> templateOrigin; // the pattern's first declaration
    std::vector<std::string> symbols; // every variant's, before the rewrite (CodeGen)
};
struct Candidate { std::string current; std::string type; std::string alternative; }; // 1.8
struct SuggestionPlan                                                                 // 1.3
{
    EntityIdentity entity; // 1.4
    unsigned sourceIndex = 0;
    Candidate candidate;   // 1.8
    std::string expectedType; // the candidate, written canonically (T v1's expectedType)
    std::map<std::string, std::vector<Edit>> edits; // by real path
    std::optional<std::string> indeterminate;       // why the plan is not executed
};
enum class FactKind : std::uint8_t { Reference, Override, Constant };               // 1.4
struct FactKey                                                                        // 1.4
{
    std::vector<std::string> contexts; // the instantiations around the fact, outermost first
    DeclarationSite spelling;
    DeclarationSite expansion;
    auto operator<=>(const FactKey&) const = default;
};
struct Fact { FactKind kind; FactKey key; std::vector<std::string> value; };        // 1.4
struct SymbolPair { std::string before; std::string after; };                       // 1.5
```
`FactKey` has no unit: each unit's facts are kept apart. `Fact::value` is sorted: for R, the target's declaration sites, or its symbol (ruling 4); for O, the overridden methods, each the same way; for K, `true`, `false` or the selected association's index.

### Task 1.1: units, verdicts, aggregation, report

**Files:** `src/analysis/ConstSuggestionCheck.cpp`, `src/analysis/ConstSuggestionInternal.hpp`, `run_test.py`.

**Interfaces.** Produces `Verdict`, `Judgement`, `UnitConfiguration` (Shared types) and:
```cpp
// §5.10: refuted if a judgement refutes, else indeterminate if one is, else verified; with the
// reason of the first judgement that has that verdict (ruling 1).
Judgement aggregate(const std::vector<Judgement>& judgements);
// 16 hexadecimal digits: xxh3 of the arguments and the working directory, each NUL-terminated.
std::string fingerprintOf(const UnitCommand& command);
```

**Tests.** A helper that later tasks' checks share, and two checks, registered after `check_const_suggestion_both_modes`:

```python
def _const_suggestion_run(
    args: list[str], env_overrides: Optional[dict[str, str]] = None
) -> Optional[tuple[dict[str, list[str]], list[str], str]]:
    """
    #217: runs the analyzer with the Clang check of const suggestions and its trace (--verbose), on
    inputs the cache key does not cover. Returns the messages of the const suggestions by function
    (a function reported twice, such as a constructor's two variants, has two), the trace's lines
    without their "[const-suggestion] " prefix, and the whole output; None when the analysis did
    not run.
    """
    result = run_analyzer_uncached(
        [*args, "--format=json", "--experimental-const-suggestion-check", "--verbose"],
        env_overrides,
    )
    if result.returncode != 0:
        return None
    try:
        diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
    except json.JSONDecodeError:
        return None
    messages: dict[str, list[str]] = {}
    for d in diagnostics:
        if str(d.get("ruleId", "")).startswith("ConstParameterNotModified"):
            function = str(d.get("location", {}).get("function", ""))
            messages.setdefault(function, []).append(str(d.get("details", {}).get("message", "")))
    output = (result.stdout or "") + (result.stderr or "")
    trace = [
        line.split("[const-suggestion] ", 1)[1]
        for line in output.splitlines()
        if "[const-suggestion] " in line
    ]
    return messages, trace, output
```

Review Focus 4, the one configuration of a file the database lists twice:

```python
def check_const_suggestion_one_configuration_per_file() -> bool:
    """
    #217: a compilation database can list a file twice, with different commands. The analysis
    loads one command per file, and the Clang check of const suggestions compiles each file with
    that one only: its trace names one configuration for each file, here the defining file,
    analyzed, and a caller that only the database lists.
    """
    print("=== Testing const suggestions with files the compilation database lists twice ===")
    fixtures = (RUN_CONFIG.test_dir / "pointer_reference-const_correctness").resolve()
    definition = fixtures / "cross-tu-const-suggestion-def.c"
    caller = fixtures / "cross-tu-const-suggestion-caller.c"
    unit = re.compile(r"read_value parameter p, unit (\S+) \[([0-9a-f]+)\]: ")
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_twice_") as tmp:
        database = Path(tmp).resolve() / "compile_commands.json"
        # A macro the files do not use: the two commands differ, the code does not.
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(fixtures),
                        "file": str(f),
                        "arguments": ["clang", *extra, "-I", str(fixtures), "-c", str(f)],
                    }
                    for f in (definition, caller)
                    for extra in ([], ["-DLISTED_TWICE"])
                ]
            )
        )
        run = _const_suggestion_run([str(definition), f"--compile-commands={database}"])
    if run is None:
        return fail_check("the analysis did not run")
    messages, trace, _ = run
    configurations: dict[str, set[str]] = {}
    for line in trace:
        match = unit.match(line)
        if match:
            configurations.setdefault(match.group(1), set()).add(match.group(2))
    checked = "checked in every file of the compilation database that sees the change"
    ok = True
    for label, good in (
        (
            "the suggestion is checked in the compilation database",
            any(checked in text for text in messages.get("read_value", [])),
        ),
        (
            "one configuration of the defining file",
            len(configurations.get(str(definition), ())) == 1,
        ),
        ("one configuration of the caller", len(configurations.get(str(caller), ())) == 1),
    ):
        print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {configurations}"))
        ok = ok and good
    print()
    return ok
```

Ruling 6, the initial compilation shared by the suggestions:

```python
def check_const_suggestion_compiles_each_file_once() -> bool:
    """
    #217: the Clang check compiles each file that sees a change once as it is, for all the
    suggestions it checks there: here two functions, defined in one file and called in another.
    Each file is compiled once before the rewrites (`--timing`), and both suggestions are checked.
    """
    print("=== Testing that each file is compiled once as it is for several const suggestions ===")
    sources = {
        "readers.h": "int read_a(int* p);\nint read_b(int* q);\n",
        "readers.c": '#include "readers.h"\n'
        "int read_a(int* p)\n{\n    return *p;\n}\n"
        "int read_b(int* q)\n{\n    return *q + 1;\n}\n",
        "user.c": '#include "readers.h"\n'
        "int use_both(void)\n{\n    int v = 1;\n    return read_a(&v) + read_b(&v);\n}\n",
    }
    calls = re.compile(r"const_suggestion\.compile\.(\w+) .*\bcalls=(\d+)")
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_once_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        # Every hotspot, not the 20 longest.
        run = _const_suggestion_run(
            [str(root / "readers.c"), str(root / "user.c"), "--timing"],
            {"CTRACE_HOTSPOT_TOP": "200"},
        )
    if run is None:
        return fail_check("the analysis did not run")
    messages, _, output = run
    compiled = {m.group(1): int(m.group(2)) for m in calls.finditer(output)}
    checked = "checked in the analyzed files only"
    ok = True
    for label, good in (
        (
            "both suggestions are checked",
            all(any(checked in t for t in messages.get(f, [])) for f in ("read_a", "read_b")),
        ),
        ("the defining file is compiled once as it is", compiled.get("defining_before") == 1),
        ("the other file is compiled once as it is", compiled.get("other_before") == 1),
    ):
        print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {compiled}"))
        ok = ok and good
    print()
    return ok
```

Failure at `64ed37e`: no trace line names a unit; the caller is compiled once per function name.

```text
check_const_suggestion_one_configuration_per_file: FAIL
=== Testing const suggestions with files the compilation database lists twice ===
  ✅ the suggestion is checked in the compilation database
  ❌ one configuration of the defining file: {}
  ❌ one configuration of the caller: {}
```

```text
check_const_suggestion_compiles_each_file_once: FAIL
=== Testing that each file is compiled once as it is for several const suggestions ===
  ✅ both suggestions are checked
  ✅ the defining file is compiled once as it is
  ❌ the other file is compiled once as it is: {'after': 4, 'other_before': 2, 'defining_before': 1}
```

- [ ] **Step 1: Add the helper and the two checks, then run them.**
  Run: the one-check command for `check_const_suggestion_one_configuration_per_file`, then for `check_const_suggestion_compiles_each_file_once`.
  Expected: FAIL, as above.
- [ ] **Step 2: Commit the tests.**
  `test(const-param): name the one configuration of a file listed twice`;
  `test(const-param): compile each file once as it is for all its suggestions`.
- [ ] **Step 3: Judgements and aggregation.** Add `Verdict`, `Judgement`, `UnitConfiguration` and `aggregate` to `ConstSuggestionInternal.hpp` and `ConstSuggestionCheck.cpp`:
  ```cpp
  Judgement aggregate(const std::vector<Judgement>& judgements)
  {
      for (const Verdict verdict : {Verdict::Refuted, Verdict::Indeterminate})
      {
          for (const Judgement& judgement : judgements)
          {
              if (judgement.verdict == verdict)
                  return judgement;
          }
      }
      return {};
  }
  ```
  `checkUnit` returns a `Judgement` instead of its `bool` and its `removed` and `notChecked` strings: T v1's `removed` text is a refutation's reason, its `notChecked` text an indeterminate one's, and a unit that passes is verified. The loop over the units keeps T v1's order and stops at the first refutation, as T v1 does. The outcome maps onto T v1's report: refuted removes the suggestion (`removed, <reason>`), indeterminate adds `kNotChecked + reason` (`not checked, <reason>`), verified adds the "checked" line and the ABI line, unchanged.
  Commit: `refactor(const-param): judge each unit and aggregate three verdicts`.
- [ ] **Step 4: Units and their configuration.** A unit is a `UnitConfiguration`: the file's real path (`canonicalPath`), its `UnitCommand` (`commandFor`, cached as T v1 caches it in `AnalyzedUnits::commands`), and `fingerprintOf(command)`:
  ```cpp
  std::string fingerprintOf(const UnitCommand& command)
  {
      std::string text;
      for (const std::string& arg : command.args)
          text.append(arg).push_back('\0');
      text.append(command.workingDir).push_back('\0');
      return llvm::utohexstr(llvm::xxh3_64bits(text), /*LowerCase=*/true, /*Width=*/16);
  }
  ```
  After each unit's judgement, the trace gives a line `<symbol> parameter <name>, unit <file> [<fingerprint>]: <verdict>[: <reason>]`, through a second `coretrace::log` call next to T v1's `trace`.
  Run: the one-check command for `check_const_suggestion_one_configuration_per_file`. Expected: PASS.
  Commit: `feat(const-param): name each unit and its configuration in the trace`.
- [ ] **Step 5: One initial compilation per unit and configuration.** `originalOf` is keyed by the unit (`file` and `fingerprint`), no longer by `(unit, function name)`, and compiles the unit once with every target of the module: `Original` keeps one entry of facts per target (`std::vector<FunctionFacts>`, indexed like `targets`). `checkUnit` reads the entry of its suggestion.
  Run: the one-check command for `check_const_suggestion_compiles_each_file_once`. Expected: PASS.
  Commit: `perf(const-param): compile each other file once as it is for all suggestions`.
- [ ] **Step 6: Task tests.** Build; unit tests; the T v1 checks and fixtures (`python3 run_test.py --jobs 8 --no-cache`). Expected: all pass; the trace lines of T v1 unchanged (`check_const_suggestion_removal_reason`).

**Validation:** every T v1 test passes unchanged; both new checks pass.

### Task 1.2: the parameter from P0

**Files:** `src/analysis/ConstSuggestionCheck.cpp`, `include/analysis/ConstParamAnalysis.hpp`, `src/analysis/ConstParamAnalysis.cpp`, `test/pointer_reference-const_correctness/const-suggestion-virtual-base.cpp` (new).

**Interfaces.** Consumes `ParameterDebugBinding::sourceIndex` and `isArtificial` (P0). Removes `ConstParamIssue::debugArgNo` (#221) and `Target::debugArgNo`.

**Tests.** D1, a new fixture (t10). `x` is reported twice, in C2 then C1; the ABI line tells them apart:

`test/pointer_reference-const_correctness/const-suggestion-virtual-base.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
// #217: the base-object constructor (C2) of a class with a virtual base takes a hidden `vtt` after
// `this`. The check rewrites the parameter the suggestion names, found by its index among the
// parameters the source declares: `x`, in both variants. `y`, which the constructor writes
// through, is not rewritten.

// strict-expectation-details: true

struct V
{
    int v = 0;
};

struct D : virtual V
{
    int a, b;
    D(int* x, int* y);
};

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'D::D(int*, int*)' is never used to modify the pointed object
// ↳ current type: int *x
// ↳ suggested type: const int *x
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_ZN1DC2EPiS0_ -> _ZN1DC2EPKiPi): code compiled separately must use the updated declaration and be recompiled

// at line 33, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'x' in function 'D::D(int*, int*)' is never used to modify the pointed object
// ↳ current type: int *x
// ↳ suggested type: const int *x
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
// ↳ ABI: the suggested type changes the symbol (_ZN1DC1EPiS0_ -> _ZN1DC1EPKiPi): code compiled separately must use the updated declaration and be recompiled
D::D(int* x, int* y) : a(*x), b(0)
{
    *y = 1;
}

int make()
{
    int p = 1, q = 2;
    D d(&p, &q);
    return d.a + q;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
```

Failure at `64ed37e`: C1's block is found, C2's is missing: T rewrites `y` for the suggestion on `x` in C2, and removes it (`read-only variable is not assignable`).

```text
new/const-suggestion-virtual-base.cpp: FAIL (4/6)
=== Testing evidence/217/stage1/tests/fixtures/new/const-suggestion-virtual-base.cpp ===
  ❌ (default) expectation #1 MISSING
  ✅ (default) expectation #2 FOUND
  ❌ (smt-z3) expectation #1 MISSING
  ✅ (smt-z3) expectation #2 FOUND
```

Review Focus 2: `const-suggestion-explicit-object.cpp` unchanged, passing at `64ed37e`.

- [ ] **Step 1: Add the fixture and run it.** Run: the one-fixture command for `const-suggestion-virtual-base.cpp`. Expected: FAIL, expectation #1 (C2) missing.
  Commit: `test(const-param): check the parameter a C2 constructor's suggestion names`.
- [ ] **Step 2: The source index.** In `Inspector::plan`, the parameter is P0's:
  ```cpp
  const ParameterDebugBinding& binding = target.binding;
  if (binding.isArtificial || !binding.sourceIndex)
  {
      facts.notRewritten = "the parameter is not found in the source";
      return;
  }
  facts.paramIndex = *binding.sourceIndex;
  ```
  `Target` carries the issue's `binding` instead of `debugArgNo`; the `implicitThis` arithmetic goes. Ruling 8: no new text.
  Run: the one-fixture command. Expected: PASS. Then `const-suggestion-explicit-object.cpp`: PASS.
  Commit: `fix(const-param): rewrite the parameter at the source index P0 gives`.
- [ ] **Step 3: Remove `debugArgNo`.** `ConstParamIssue::debugArgNo`, its padding and its assignment in `ConstParamAnalysis.cpp` go: nothing reads them.
  Commit: `refactor(const-param): drop the debug number that the source index replaces`.
- [ ] **Step 4: Task tests.** As Task 1.1, Step 6.

**Validation:** D1 passes; explicit-object unchanged; every T v1 test passes.

### Task 1.3: planning and plan scope (§5.5)

**Files:** `src/analysis/ConstSuggestionPlan.cpp` (new), `src/analysis/ConstSuggestionCheck.cpp`, `src/analysis/ConstSuggestionInternal.hpp`, `CMakeLists.txt`, `test/pointer_reference-const_correctness/const-suggestion-macro-shared-type.c` (new), `run_test.py`.

**Interfaces.** Produces:
```cpp
// The ranges of the macro definitions a compilation reads, by real path: from the macro's name
// to its last token, as offsets (D11).
using MacroDefinitions = std::map<std::string, std::vector<std::pair<unsigned, unsigned>>>;
// Records them, for the compilation that plans (§5.5).
std::unique_ptr<clang::PPCallbacks> recordMacroDefinitions(const clang::SourceManager& sm,
                                                           MacroDefinitions& out);
// Plans the rewrite of the parameter `binding` names, in `function`, located in the defining
// unit's initial compilation by the suggestion's symbol (T v1's GetDeclForMangledName).
SuggestionPlan plan(clang::ASTContext& context, const MacroDefinitions& macros,
                    const clang::FunctionDecl& function, const ParameterDebugBinding& binding);
```
`Edit` moves to `ConstSuggestionInternal.hpp`. `SuggestionPlan` holds here `sourceIndex`, `expectedType`, `edits` and `indeterminate`; Task 1.4 adds `entity`, Task 1.8 `candidate`.

**Tests.** D5, a new fixture (t3/m.c):

`test/pointer_reference-const_correctness/const-suggestion-macro-shared-type.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// #217: a macro argument can write the type of several parameters. Rewriting the type of `src`
// where it is written would rewrite `dst` too, which copy_one writes through: the rewrite would
// reach beyond the suggestion. The suggestion is kept, not checked.

// strict-expectation-details: true

#define DECLARE_COPY(name, T) int name(T src, T dst)

// at line 15, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'src' in function 'copy_one' is never used to modify the pointed object
// ↳ current type: int *src
// ↳ suggested type: const int *src
// ↳ not checked with Clang: the parameter's type is written in a macro argument that writes other parameters too
DECLARE_COPY(copy_one, int*)
{
    *dst = *src;
    return 0;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
```

D11 (t13), a third case of `check_const_suggestion_line_endings`: spaces between the backslash and the line break, which Clang reads as a continuation, with a warning. Run as a check, not a fixture: a fixture's trailing spaces would not survive formatting and review.

```diff
--- a/run_test.py
+++ b/run_test.py
@@ -2,8 +2,10 @@
     """
     #217: a type written on a continuation line of a #define is in the macro definition, whether
-    the file ends its lines with LF or CRLF: rewriting it would change every expansion of the macro,
-    here a function that writes through p. The suggestion is kept as not checked with both.
+    the file ends its lines with LF or CRLF, or puts spaces between the backslash and the line
+    break, which Clang accepts with a warning: rewriting it would change every expansion of the
+    macro, here a function that writes through p. The suggestion is kept as not checked in each
+    case.
     """
-    print("=== Testing a macro definition's continuation lines, with LF and CRLF ===")
+    print("=== Testing a macro definition's continuation lines, with LF, CRLF and spaces ===")
     source = (
         "#define PARAM(type) type p\n"
@@ -16,7 +18,11 @@
     ok = True
     with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_line_endings_") as tmp:
-        for label, newline in (("LF", "\n"), ("CRLF", "\r\n")):
-            path = Path(tmp).resolve() / f"reader-{label.lower()}.c"
-            path.write_bytes(source.replace("\n", newline).encode("utf-8"))
+        for label, contents in (
+            ("LF", source),
+            ("CRLF", source.replace("\n", "\r\n")),
+            ("spaces after the backslash", source.replace("\\\n", "\\  \n")),
+        ):
+            path = Path(tmp).resolve() / f"reader-{label.lower().replace(' ', '-')}.c"
+            path.write_bytes(contents.encode("utf-8"))
             # The file's bytes are an input the cache key does not cover.
             result = run_analyzer_uncached(
```

Failure at `64ed37e`: both suggestions are removed. D5: the edit of the macro argument rewrites `dst` too, which copy_one writes. D11: the text scan does not see the continuation, so the macro body is rewritten, and so is write_second.

```text
new/const-suggestion-macro-shared-type.c: FAIL (2/4)
=== Testing evidence/217/stage1/tests/fixtures/new/const-suggestion-macro-shared-type.c ===
  ❌ (default) expectation #1 MISSING
  ❌ (smt-z3) expectation #1 MISSING
```

```text
check_const_suggestion_line_endings: FAIL
=== Testing a macro definition's continuation lines, with LF, CRLF and spaces ===
  ✅ LF
  ✅ CRLF
  ❌ spaces after the backslash: None
```

`const-suggestion-macros.c` and `const-suggestion-macro-params.c` stay unchanged and pass at `64ed37e`.

- [ ] **Step 1: Add both tests and run them.** Run: the one-fixture command for `const-suggestion-macro-shared-type.c`, and the one-check command for `check_const_suggestion_line_endings`. Expected: FAIL, as above.
  Commits: `test(const-param): keep a suggestion whose macro argument types two parameters`; `test(const-param): a backslash and spaces still continue a macro definition`.
- [ ] **Step 2: Move the planning.** `Planner`, `writtenType`, `expectedType` and the body of `Inspector::plan` move to `ConstSuggestionPlan.cpp`, behind `plan()`; `FunctionFacts` keeps what the other compilations fill. No behavior changes.
  Run: the T v1 macro fixtures. Expected: PASS. Commit: `refactor(const-param): move the planning of a suggestion to its own file`.
- [ ] **Step 3: Macro definitions from the preprocessor (D11).** `recordMacroDefinitions` returns a `clang::PPCallbacks` whose `MacroDefined` records `[getDefinitionLoc(), getDefinitionEndLoc()]` of each macro written in a file (real path, offsets). `InspectAction::CreateASTConsumer` adds it to the preprocessor of the defining unit's initial compilation. `Planner::replace` asks the recorded ranges whether the offset it rewrites lies in a macro definition; the text scan `inMacroDefinition` goes.
  Run: `check_const_suggestion_line_endings`. Expected: PASS. Commit: `fix(const-param): find macro definitions with the preprocessor, not the text`.
- [ ] **Step 4: The plan's scope (D5).** An edit whose position comes from a macro argument (`isMacroArgExpansion`) serves only the parameter planned. `plan()` walks the other parameters of each redeclaration with the same `TypeLoc` walk as `Planner::plan`: one whose type's spelling location is an edit's position gives `the parameter's type is written in a macro argument that writes other parameters too`. Then, only when an edit comes from a macro argument, a `RecursiveASTVisitor` over the translation unit checks every other `DeclaratorDecl` and `TypedefNameDecl`: one whose type starts at an edit's position gives `the parameter's type is written in a macro argument that writes other declarations too`. Either way the plan is indeterminate and nothing is rewritten.
  Run: the one-fixture command for `const-suggestion-macro-shared-type.c`. Expected: PASS. Commit: `fix(const-param): leave a rewrite that reaches another declaration unchecked`.
- [ ] **Step 5: Task tests.** As Task 1.1, Step 6.

**Validation:** D5 and D11 pass; T v1's macro tests unchanged.

### Task 1.4: compilations, facts R, O and K, scope after the rewrite (§5.3, §5.4, §5.6), judgement steps 1 to 6

**Files:** `src/analysis/ConstSuggestionFacts.cpp` (new), `src/analysis/ConstSuggestionCheck.cpp`, `src/analysis/ConstSuggestionPlan.cpp`, `src/analysis/ConstSuggestionInternal.hpp`, `CMakeLists.txt`, `run_test.py`, `test/pointer_reference-const_correctness/const-suggestion-facts-without-code-change.cpp` (new).

**Interfaces.** Adds `DeclarationSite`, `SourceLinkage`, `EntityIdentity`, `FactKind`, `FactKey`, `Fact` (Shared types) and `SuggestionPlan::entity`. Produces:
```cpp
// What one compilation shows (§5.3, §5.6). R and K cover the whole unit; the rest is kept for
// each plan given, in their order.
struct UnitFacts
{
    std::string failure;   // the compilation could not run (T v1's Compilation::failure)
    bool complete = false; // the AST holds the whole unit: no fatal error
    std::vector<Diagnostic> diagnostics; // errors and tracked warnings, in Clang's order
    std::shared_ptr<const const_suggestion::ModuleText> code; // prepared for the comparator
    std::map<std::string, llvm::GlobalValue::LinkageTypes> defined; // symbols with a body
    std::vector<Fact> facts; // R and K
    std::map<DeclarationSite, std::string> writtenTypes; // declarations of the rewritten files
    struct Entity
    {
        bool found = false;
        std::vector<std::string> symbols;     // every variant's, in this compilation
        std::vector<DeclarationSite> sites;   // its declarations here
        std::vector<Fact> overrides;          // O, for the methods named like it
        std::vector<DeclarationSite> casts;   // explicit casts of its address (Task 1.7)
        std::vector<std::string> stale;       // T v1's "declared in <file>:<line> with type <type>"
        std::string printedSignature;         // T v1's, for __PRETTY_FUNCTION__
    };
    std::vector<Entity> entities;
};
// Compiles `unit` with `remapped` files, and inspects it for `plans`.
UnitFacts inspect(const UnitConfiguration& unit, const std::map<std::string, std::string>& remapped,
                  const std::vector<const SuggestionPlan*>& plans, bool timing);
```
The defining unit's initial compilation also plans: `inspect` locates each suggestion's function by its symbol there, calls `plan()`, then collects the facts with the identities planned.

**Tests.** D2 (t1) and D3 (t2, ruling 5), two new checks, registered after the checks of Task 1.1:

```python
def check_const_suggestion_operator_used_elsewhere() -> bool:
    """
    #217: a function is found by its declaration in every file that sees the change, not by its
    name: an operator has no identifier. Here a member operator and a method, declared in a header,
    defined in one file and called in another: both suggestions are checked in both files.
    """
    print("=== Testing const suggestions for a member operator used in another file ===")
    sources = {
        "vec.hpp": "struct Vec\n{\n    int x;\n    bool operator==(Vec& o) const;\n"
        "    int dot(Vec& o) const;\n};\n",
        "vec.cpp": '#include "vec.hpp"\n'
        "bool Vec::operator==(Vec& o) const\n{\n    return x == o.x;\n}\n"
        "int Vec::dot(Vec& o) const\n{\n    return x * o.x;\n}\n",
        "use.cpp": '#include "vec.hpp"\n'
        "int use(Vec a, Vec b)\n{\n    return (a == b) + a.dot(b);\n}\n",
    }
    checked = "checked in the analyzed files only"
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_operator_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        run = _const_suggestion_run([str(root / "vec.cpp"), str(root / "use.cpp")])
    if run is None:
        return fail_check("the analysis did not run")
    messages, trace, _ = run
    ok = True
    for label, function in (("operator==", "_ZNK3VeceqERS_"), ("dot", "_ZNK3Vec3dotERS_")):
        texts = messages.get(function, [])
        good = (
            len(texts) == 1
            and checked in texts[0]
            and f"{function} parameter o: checked in 2 file(s)" in trace
        )
        print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {texts}"))
        ok = ok and good
    print()
    return ok
```

```python
def check_const_suggestion_homonyms_across_files() -> bool:
    """
    #217: in C, a function with internal linkage and a function of the same name with external
    linkage in another file are two functions: neither file declares the other's. Whichever of the
    two gets the suggestion, the other file does not refute it, and it is checked.
    """
    print("=== Testing const suggestions for C homonyms with internal and external linkage ===")
    sources = {
        "static-reads.c": "static int helper(int* p)\n{\n    return *p;\n}\n"
        "int use_helper(void)\n{\n    int v = 1;\n    return helper(&v);\n}\n",
        "external-writes.c": "int helper(int* p)\n{\n    *p = 2;\n    return 0;\n}\n",
        "external-reads.c": "int reader(int* p)\n{\n    return *p;\n}\n",
        "static-writes.c": "static int reader(int* p)\n{\n    *p = 3;\n    return 0;\n}\n"
        "int use_reader(void)\n{\n    int v = 1;\n    return reader(&v);\n}\n",
    }
    checked = "checked in the analyzed files only"
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_homonyms_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        for label, files, function in (
            (
                "a static reader, an external writer",
                ["static-reads.c", "external-writes.c"],
                "helper",
            ),
            (
                "an external reader, a static writer",
                ["external-reads.c", "static-writes.c"],
                "reader",
            ),
        ):
            run = _const_suggestion_run([str(root / f) for f in files])
            texts = run[0].get(function, []) if run else []
            good = len(texts) == 1 and checked in texts[0]
            print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {texts}"))
            ok = ok and good
    print()
    return ok
```

The scope after the rewrite (§5.6), a third check: a header whose text another file reads as other declarations (ruling 11):

```python
def check_const_suggestion_header_read_differently() -> bool:
    """
    #217: a header can mean other declarations in another file, through a macro that file defines
    otherwise. Here pair.h declares read_value in def.c, and a global pointer in slot.c: rewriting
    the type pair.h writes changes that global's type too, although the generated code does not
    show it. The rewrite reaches beyond the suggestion: it is kept, not checked.
    """
    print("=== Testing a const suggestion whose header means other declarations elsewhere ===")
    sources = {
        "pair.h": "PAIR(read_value, int*)\n",
        "def.c": '#define PAIR(name, type) int name(type p);\n#include "pair.h"\n#undef PAIR\n'
        "int read_value(int* p)\n{\n    return *p;\n}\n",
        "slot.c": '#define PAIR(name, type) type name##_slot;\n#include "pair.h"\n',
    }
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_xmacro_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        run = _const_suggestion_run([str(root / "def.c"), str(root / "slot.c")])
        expected = (
            "not checked with Clang: the rewrite also changes the type of read_value_slot "
            f"({root / 'pair.h'}:1)"
        )
    texts = run[0].get("read_value", []) if run else []
    good = len(texts) == 1 and expected in texts[0]
    detail = "" if good else f": {texts}"
    print(f"  {'✅' if good else '❌'} the global slot.c declares{detail}")
    print()
    return good
```

A new fixture: facts refute without a change of the generated code. T v1 never sees these, since it compares only code:

`test/pointer_reference-const_correctness/const-suggestion-facts-without-code-change.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
// #217: a call or a constant condition that the suggested type changes refutes the suggestion,
// even when the generated code stays the same:
// - sized: sizeof names sized(bool) before the rewrite and sized(const int*) after it; both return
//   int, so the size, and the code, stay the same;
// - typed: if constexpr tests the parameter's own type, and both branches are the same code.

// not contains: parameter 'p' in function 'sized(int*)'
// not contains: parameter 'p' in function 'typed(int*)'
#include <type_traits>

int sized(int* p)
{
    return *p;
}

int sized(bool flag);

int size_of_call(const int* q)
{
    return static_cast<int>(sizeof(sized(q)));
}

int typed(int* p)
{
    if constexpr (std::is_const_v<std::remove_pointer_t<decltype(p)>>)
        return *p;
    else
        return *p;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
```

Failure at `64ed37e`. D2: `operator==` is removed, "the methods it overrides change": T v1 recognizes a method by its identifier after the rewrite, and an operator has none. D3: both removed, "declared in b.c:1 / d.c:1 with type int *": T v1 takes a homonym for the function, whatever its linkage. The header: checked, since the global's new type leaves the IR as it was. The fixture: both kept as checked.

```text
check_const_suggestion_operator_used_elsewhere: FAIL
=== Testing const suggestions for a member operator used in another file ===
  ❌ operator==: []
  ✅ dot
```

```text
check_const_suggestion_homonyms_across_files: FAIL
=== Testing const suggestions for C homonyms with internal and external linkage ===
  ❌ a static reader, an external writer: []
  ❌ an external reader, a static writer: []
```

```text
check_const_suggestion_header_read_differently: FAIL
=== Testing a const suggestion whose header means other declarations elsewhere ===
  ❌ the global slot.c declares: ["\t[ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'read_value' is never used to modify the pointed object\n\t\t ↳ current type: int *p\n\t\t ↳ suggested type: con …
```

```text
new/const-suggestion-facts-without-code-change.cpp: FAIL (2/6)
=== Testing evidence/217/stage1/tests/fixtures/new/const-suggestion-facts-without-code-change.cpp ===
  ❌ (default) negative expectation #1 FOUND (unexpected)
  ❌ (default) negative expectation #2 FOUND (unexpected)
  ❌ (smt-z3) negative expectation #1 FOUND (unexpected)
  ❌ (smt-z3) negative expectation #2 FOUND (unexpected)
```

Unchanged and passing at `64ed37e`: `const-suggestion-same-code.cpp`, `const-suggestion-override-elsewhere.cpp`, `pick` in `const-suggestion-deduction-refused.cpp`, `generic_selection` in `const-suggestion-must-compile.c`.

- [ ] **Step 1: Add the four tests and run them.** Expected: FAIL, as above.
  Commits: `test(const-param): check a member operator's suggestion in the file that calls it`; `test(const-param): a C homonym with another linkage is another function`; `test(const-param): a changed call or constant refutes even with the same code`; `test(const-param): keep a rewrite that changes another file's declaration`.
- [ ] **Step 2: Move the compilation.** `DiagnosticCollector`, `Inspector`, `InspectAction`, `compile` and `scan` move to `ConstSuggestionFacts.cpp`. No behavior changes. Run: the T v1 checks and fixtures. Expected: PASS.
  Commit: `refactor(const-param): move the compilation and its inspection to their own file`.
- [ ] **Step 3: The entity's identity (§3, §5.5).** `plan()` describes the located function: its redeclarations' sites (the expansion location of each name, as a real path and an offset), its formal linkage (`External` for external or module linkage, `Internal` for internal or unique-external, `None` otherwise), its template pattern's first declaration, and its variants' symbols, from `CodeGenerator::GetMangledName` (a constructor's `Ctor_Complete` and `Ctor_Base`, a destructor's `Dtor_Deleting`, `Dtor_Complete` and `Dtor_Base`, otherwise the function's).
  In every other compilation, the entity is the function that has one of those sites, once mapped back to the original text through the plan's edits:
  ```cpp
  // An offset of the rewritten text, in the original one. An offset inside inserted text maps
  // to the edit's position.
  std::size_t originalOffset(const std::vector<Edit>& sortedEdits, std::size_t offset)
  {
      std::size_t inserted = 0; // text the edits before `offset` add
      std::size_t removed = 0;  // and remove
      for (const Edit& edit : sortedEdits)
      {
          const std::size_t at = edit.offset + inserted - removed;
          if (offset < at)
              break;
          if (offset < at + edit.text.size())
              return edit.offset;
          inserted += edit.text.size();
          removed += edit.length;
      }
      return offset - inserted + removed;
  }
  ```
  A unit that shares none of those sites declares the entity only through the linker: a function with external linkage whose symbol is one of the entity's. T v1's name and symbol test (`isTarget`, `locate`'s `withoutConst` search) goes. A unit sees the change (§5.2) through a reference in its analyzed module only if the entity has external linkage.
  Run: both checks. Expected: PASS. Commit: `fix(const-param): find the function by its declarations, not by its name`.
- [ ] **Step 4: Facts R, O and K (§5.4).** The `Inspector` visits the whole unit, template instantiations included:
  - R: `DeclRefExpr`, `MemberExpr`, `CXXConstructExpr`, `CXXNewExpr` and `CXXDeleteExpr` that name a function. Key: the instantiations around the expression (each by its symbol, outermost first), and its spelling and expansion sites, mapped back. Value (ruling 4): the target's declaration sites, mapped back; for a template specialization or an implicit function, its symbol;
  - O: for each method with the entity's `DeclarationName`, the methods it overrides (`overridden_methods()`), each by the same value as R. Key: the method's site;
  - K: each `IfStmt` with `isConstexpr()` and a condition that is not value-dependent: `true` or `false`, from `EvaluateKnownConstInt`; each `GenericSelectionExpr` that is not result-dependent: `getResultIndex()`.
  A symbol comes from `CodeGenerator::GetMangledName`, cached by declaration. A fact is compared only when its key's instantiations all map to the other compilation: here, through the entity's own symbol pairs (Task 1.5 adds the others). An R fact is unchanged when its value after contains its value before (ruling 4); O and K must be equal. T v1's comparison of overrides by qualified name goes.
  Judgement step 6, after T v1's diagnostics (steps 4 and 5): the first changed fact refutes, with its text (Texts).
  Run: the new fixture, then `const-suggestion-same-code.cpp`, `const-suggestion-override-elsewhere.cpp`, `const-suggestion-deduction-refused.cpp` and `const-suggestion-must-compile.c`. Expected: PASS.
  Commit: `feat(const-param): refute a suggestion that changes a call, override or constant`.
- [ ] **Step 5: Scope after the rewrite (§5.6), steps 2 and 3.** Each compilation records `writtenTypes`: the canonical type (`writtenType`) of every declaration whose name's expansion location lies in a file that some plan rewrites (`DeclaratorDecl`, `TypedefNameDecl`), neither implicit nor instantiated, by its site. After the rewrite, every site keeps its type, except the planned parameters and the entity's redeclarations; a planned parameter must have the candidate (T v1's `the rewritten declaration does not get the suggested type`). Otherwise step 3: `the rewrite also changes the type of <name> (<file>:<line>)`. A compilation with a fatal error (`hasFatalErrorOccurred()`) has an incomplete inspection, step 2: `<file> could not be inspected once rewritten`. T's compilations set no error limit (`DiagnosticOptions::ErrorLimit = 0`): "too many errors" would otherwise make a rewrite that breaks many uses incomplete instead of refuted, where T v1 refutes it. Step 2 then guards only a fatal error that a rewrite brings, such as a missing file: no input of the suite reaches it (ruling 12). The judgement now runs steps 1 to 6 in the order of §5.8; T v1's code comparison stays step 7 until Task 1.5.
  Run: `check_const_suggestion_header_read_differently`. Expected: PASS. Commit: `feat(const-param): leave a rewrite that changes another declaration unchecked`.
- [ ] **Step 6: Task tests.** As Task 1.1, Step 6.

**Validation:** D2, D3, the header check and the new fixture pass; the refutations by R, O and K of T v1's tests unchanged.

### Task 1.5: correspondences and the comparator's pairs (§5.7, §5.9), judgement step 7

**Files:** `src/analysis/ConstSuggestionMatch.cpp` (new), `src/analysis/ConstSuggestionFacts.cpp`, `src/analysis/ConstSuggestionCode.cpp`, `src/analysis/ConstSuggestionCheck.cpp`, `src/analysis/ConstSuggestionInternal.hpp`, `include/analysis/ConstSuggestionCheck.hpp`, `CMakeLists.txt`, `test/pointer_reference-const_correctness/const-suggestion-inheriting-constructor.cpp` (new), `const-suggestion-deduction-refused.cpp` and `const-suggestion-printed-signature.c` (expectations changed).

**Interfaces.** Adds `SymbolPair`. `UnitFacts::Entity` gains what the correspondences read:
```cpp
// A symbol that the rewrite may rename with the entity, and what it is in this compilation.
struct Named
{
    std::string symbol;
    std::string key; // what pairs it: its kind, its site mapped back, and its enclosing symbol
};
std::vector<Named> variants;   // the entity's: "C1", "C2", "D0"... or "" for a function
std::vector<Named> inheriting; // the constructors inheriting it: derived class and variant
std::vector<Named> nested;     // lambdas, local classes (methods, _ZTI, _ZTS, _ZTV), static
                               // locals and their guards, in the entity or a specialization
```
An R fact's value also carries `template:<site>` when its target is a template specialization. Produces:
```cpp
// The pairs established between a unit's initial and modified compilations for one entity
// (§5.7). `missing` receives the first reason a correspondence could not be established.
std::vector<SymbolPair> correspond(const UnitFacts& before, const UnitFacts& after,
                                   std::size_t entity, std::string& missing);
// ConstSuggestionCode: the pairs replace `rewritten` and `renamedTo` (ruling 9).
std::optional<std::string> codeDifference(const ModuleText& was, const ModuleText& now,
                                          const std::vector<SymbolPair>& established,
                                          const PrintedSignature* signature,
                                          std::vector<std::string>* accepted);
```
`rewrittenCodeDifference` in `ConstSuggestionCheck.hpp` takes the pairs the same way.

**Tests.** D7, a new fixture (t8 and three template cases, ruling 7):

`test/pointer_reference-const_correctness/const-suggestion-inheriting-constructor.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
// #217: a class that inherits a constructor (`using Base::Base`) gets constructors of its own,
// whose symbols name the inherited constructor's parameter types: the rewrite renames them too.
// The check pairs them before and after the rewrite through the constructor they inherit, so
// each suggestion is checked: a plain class, a class template as the base, a class template as
// the derived class, and a constructor template.

// strict-expectation-details: true

// at line 18, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'Base::Base(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct Base
{
    int v;
    Base(int* p) : v(*p) {}
};

struct Derived : Base
{
    using Base::Base;
    int w = 0;
};

int make_derived(int x)
{
    Derived d(&x);
    return d.v;
}

// at line 41, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'TemplateBase<int>::TemplateBase(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
template <typename T> struct TemplateBase
{
    T v;
    TemplateBase(T* p) : v(*p) {}
};

struct FromTemplate : TemplateBase<int>
{
    using TemplateBase::TemplateBase;
    int w = 0;
};

int make_from_template(int x)
{
    FromTemplate d(&x);
    return d.v;
}

// at line 64, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'PlainBase::PlainBase(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct PlainBase
{
    int v;
    PlainBase(int* p) : v(*p) {}
};

template <typename T> struct TemplateDerived : PlainBase
{
    using PlainBase::PlainBase;
    T w = 0;
};

int make_template_derived(int x)
{
    TemplateDerived<int> d(&x);
    return d.v;
}

// at line 87, column 0
// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'GenericBase::GenericBase<int>(int*)' is never used to modify the pointed object
// ↳ current type: int *p
// ↳ suggested type: const int *p
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside
struct GenericBase
{
    int v;
    template <typename T> GenericBase(T* p) : v(*p) {}
};

struct FromGeneric : GenericBase
{
    using GenericBase::GenericBase;
    int w = 0;
};

int make_from_generic(int x)
{
    FromGeneric d(&x);
    return d.v;
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
```

D14, `call` in `const-suggestion-deduction-refused.cpp`: kept, not checked, the reason naming the specializations; `pick` stays removed:

```diff
--- a/test/pointer_reference-const_correctness/const-suggestion-deduction-refused.cpp
+++ b/test/pointer_reference-const_correctness/const-suggestion-deduction-refused.cpp
@@ -1,14 +1,29 @@
 // SPDX-License-Identifier: Apache-2.0
 // #217: specializations of the rewritten template whose deduced type changes are paired only when
-// the correspondence is unique, and their code is still compared.
+// the correspondence is one to one, and what they select is still compared.
 // - call: g passes G& and h passes const G&; after the rewrite both deduce G, so two
-//   specializations become one. No unique correspondence: both suggestions are removed. The
-//   lambdas are noexcept, so that all three specializations get the same attributes: only the
-//   correspondence tells them apart.
-// - pick: the lvalue's specialization is paired, but its code changes with what F deduces.
+//   specializations become one. Without a one-to-one correspondence, nothing shows that the
+//   rewrite keeps or changes what they do: both suggestions are kept, not checked, and the reason
+//   names the specializations. The lambdas are noexcept, so that all three specializations get
+//   the same attributes: only the correspondence tells them apart.
+// - pick: the lvalue's specialization is paired, and the condition of its `if constexpr` changes
+//   with what F deduces: the suggestion is removed.
 
-// not contains: in function 'int call<
+// strict-expectation-details: true
 // not contains: in function 'int pick<
 
+// at line 28, column 0
+// [ !Info! ] ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'f' in function 'int call<from_temporary()::$_0>(from_temporary()::$_0&&)' is an rvalue reference and is only read, or passed where a reference to const is expected
+// ↳ consider passing by value (<anonymous type> f) or const reference (const <anonymous type> &f)
+// ↳ current type: <anonymous type> &&f
+// ↳ the callee's body is not checked: it may still modify the referred object through a cast
+// ↳ not checked with Clang: 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' and 'int call<from_lvalues()::$_0 const&>(from_lvalues()::$_0 const&)' both become 'int call<from_lvalues()::$_0>(from_lvalues()::$_0 const&)': their correspondence is not one to one
+
+// at line 28, column 0
+// [ !Info! ] ConstParameterNotModified.Reference: parameter 'f' in function 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' is only read, or passed where a reference to const is expected
+// ↳ current type: <anonymous type> &f
+// ↳ suggested type: const <anonymous type> &f
+// ↳ the callee's body is not checked: it may still modify the referred object through a cast
+// ↳ not checked with Clang: 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' and 'int call<from_lvalues()::$_0 const&>(from_lvalues()::$_0 const&)' both become 'int call<from_lvalues()::$_0>(from_lvalues()::$_0 const&)': their correspondence is not one to one
 template <typename F> int call(F&& f)
 {
```

D16, `signature_size` in `const-suggestion-printed-signature.c`: kept, not checked. The expectation stops after `signature_size:`, the comparator's text that follows names IR values:

```diff
--- a/test/pointer_reference-const_correctness/const-suggestion-printed-signature.c
+++ b/test/pointer_reference-const_correctness/const-suggestion-printed-signature.c
@@ -2,8 +2,8 @@
 // #217: __PRETTY_FUNCTION__ prints the function's signature, which the suggested type changes. The
 // suggestion is kept, and the message says what the program prints differently. Any other use of
-// that text, such as its size, is a change of the generated code.
+// that text, such as its size, changes the generated code in a way that no reference, override or
+// constant condition explains: that suggestion is kept, not checked.
 
 // strict-expectation-details: true
-// not contains: parameter 'p' in function 'signature_size'
 // not contains: ↳ ABI:
 #include <stdio.h>
@@ -21,4 +21,9 @@
 }
 
+// at line 28, column 0
+// [ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'signature_size' is never used to modify the pointed object
+// ↳ current type: int *p
+// ↳ suggested type: const int *p
+// ↳ not checked with Clang: the generated code changes, and no reference, override or constant condition explains it: code changed in signature_size:
 int signature_size(int* p)
 {
```

Failure at `64ed37e`: the four inherited constructors and both `call` suggestions are removed ("symbols no longer used"), `signature_size` too ("the generated code changes").

```text
new/const-suggestion-inheriting-constructor.cpp: FAIL (2/10)
=== Testing evidence/217/stage1/tests/fixtures/new/const-suggestion-inheriting-constructor.cpp ===
  ❌ (default) expectation #1 MISSING
  ❌ (default) expectation #2 MISSING
  ❌ (default) expectation #3 MISSING
  ❌ (default) expectation #4 MISSING
  ❌ (smt-z3) expectation #1 MISSING
  ❌ (smt-z3) expectation #2 MISSING
  ❌ (smt-z3) expectation #3 MISSING
  ❌ (smt-z3) expectation #4 MISSING
```

```text
changed/const-suggestion-deduction-refused.cpp: FAIL (4/8)
=== Testing evidence/217/stage1/tests/fixtures/changed/const-suggestion-deduction-refused.cpp ===
  ❌ (default) expectation #1 MISSING
  ❌ (default) expectation #2 MISSING
  ✅ (default) negative expectation #1 NOT FOUND (as expected)
  ❌ (smt-z3) expectation #1 MISSING
  ❌ (smt-z3) expectation #2 MISSING
  ✅ (smt-z3) negative expectation #1 NOT FOUND (as expected)
```

```text
changed/const-suggestion-printed-signature.c: FAIL (6/8)
=== Testing evidence/217/stage1/tests/fixtures/changed/const-suggestion-printed-signature.c ===
  ✅ (default) expectation #1 FOUND
  ❌ (default) expectation #2 MISSING
  ✅ (default) negative expectation #1 NOT FOUND (as expected)
  ✅ (smt-z3) expectation #1 FOUND
  ❌ (smt-z3) expectation #2 MISSING
  ✅ (smt-z3) negative expectation #1 NOT FOUND (as expected)
```

Unchanged and passing at `64ed37e`: `const-suggestion-kept*.cpp`, `const-suggestion-kept.c`, `const-suggestion-abi*.cpp`, `const-suggestion-deduction-kept.cpp`, `print_and_read` in `const-suggestion-printed-signature.c`. Their renamed symbols, measured before and after the rewrite (`evidence/217/stage1/nested-probe/symbols.txt`): the function; `_ZTI` and `_ZTS` of a local class; a lambda's call operator; the specializations of the rewritten template and the lambda instantiated in each.

- [ ] **Step 1: Add the fixture, change the two expectations, run the three.** Expected: FAIL, as above.
  Commits: `test(const-param): check a suggestion through the constructors that inherit it`; `test(const-param): keep the suggestions whose specializations merge, unchecked`; `test(const-param): keep a suggestion whose code change nothing explains, unchecked`.
- [ ] **Step 2: Symbols from compilation facts.** The `Inspector` records, in every compilation:
  - `variants`: the entity's variants, by `GlobalDecl` (Task 1.4, Step 3), keyed by variant;
  - `inheriting`: each `CXXConstructorDecl` with `isInheritingConstructor()` whose `getInheritedConstructor().getConstructor()` is the entity, or a specialization of its pattern; its `Ctor_Complete` and `Ctor_Base` symbols, keyed by the derived class (its mangled type, `MangleContext::mangleCanonicalTypeName`) and the variant. `evidence/217/stage1/d7-probe/` shows these symbols equal to the IR's, before and after, in the four cases of the fixture;
  - `nested`: in the entity, and in each function that a specialization of its template defines, every lambda's call operator, every local class's methods (by variant) and its `_ZTI`, `_ZTS` and `_ZTV` (`MangleContext::mangleCXXRTTI`, `mangleCXXRTTIName`, `mangleCXXVTable`), every static local variable and its guard (`mangleStaticGuardVariable`); keyed by kind, site mapped back, and the enclosing function's symbol. `evidence/217/stage1/d7-probe/nested-ast.txt` shows these symbols equal to the IR's (`nested-output.txt`).
  Commit: `feat(const-param): record the symbols a rewrite may rename, with their declarations`.
- [ ] **Step 3: `correspond` (§5.7)**, in `ConstSuggestionMatch.cpp`, in this order:
  1. the entity's variants, by variant;
  2. the inheriting constructors, by derived class and variant;
  3. the instantiation contexts, from the R facts whose instantiations already map. For each such fact whose target changes, the pair (target before, target after) is a candidate if both are specializations of one template (`template:<site>` equal). The candidates of every fact are then checked together: a target before with two targets after, or two targets before with one after, gives `missing` (Texts, §5.7), and no pair for them. Each new pair maps more instantiations, and the step repeats until no pair is added;
  4. the nested symbols, by key, once their enclosing symbols are paired.
  A pair is established only from these facts; the names of the symbols play no part.
  Commit: `feat(const-param): establish the correspondences of a rewrite from its facts`.
- [ ] **Step 4: The comparator takes the pairs, step 7.** `codeDifference` renames the symbols of `established` before comparing; every symbol that only one side has, and that no pair covers, is listed in its difference, as T v1 lists it (`symbols no longer used: …; symbols used only after the rewrite: …`). The masked encodings, `withoutConst`, `variantOf`, `templateOf` and `namesTemplate` go (ruling 9). The ABI line takes the symbol after the rewrite from the pair of the suggestion's own symbol. In the judgement, `correspond` runs before step 6: the facts are compared through its pairs, and those in an instantiation it could not pair are left out. After step 6, its `missing` makes the unit indeterminate; otherwise a difference of the comparator makes it indeterminate with `the generated code changes, and no reference, override or constant condition explains it: <difference>`. Step 7 never refutes.
  Run: the three tests of Step 1, then every T v1 fixture with the option. Expected: PASS; each T v1 "checked" stays checked; `pick` is refuted by its K fact, the R and O refutations of `const-suggestion-same-code.cpp` and `const-suggestion-override-elsewhere.cpp` by their facts.
  Commit: `fix(const-param): compare the code through established pairs, never refuting`.
- [ ] **Step 5: Task tests.** As Task 1.1, Step 6.

**Validation:** D7, D14 and D16 pass; every T v1 "checked" stays verified.

### Task 1.6: collisions, stale declarations, link-edition assumption (§5.10)

**Files:** `src/analysis/ConstSuggestionMatch.cpp`, `src/analysis/ConstSuggestionFacts.cpp`, `src/analysis/ConstSuggestionCheck.cpp`, `README.md`, `run_test.py`.

**Interfaces.** `UnitFacts::Entity` gains:
```cpp
// After the rewrite: a symbol the entity takes that another entity defines here, or another
// entity's declaration merged into it (ruling 3).
struct Collision
{
    std::string symbol;
    DeclarationSite site;                   // the other entity's definition, or declaration
    std::optional<llvm::GlobalValue::LinkageTypes> linkage; // none for a declaration
};
std::vector<Collision> collisions;
```
and produces, in `ConstSuggestionMatch.cpp`:
```cpp
// §5.10: the verdict of the collisions that every unit found, after the units' judgements.
Judgement judgeCollisions(const std::vector<const UnitFacts*>& units, std::size_t entity,
                          const std::string& definingFile);
```

**Tests.** D4 (t9), the other file analyzed or only in the compilation database:

```python
def check_const_suggestion_overload_defined_elsewhere() -> bool:
    """
    #217: rewritten, f(int*) takes the symbol of the overload f(const int*), which another file
    defines. The two definitions collide only if the files are linked together, which the check
    does not know: the suggestion is kept, not checked, and the reason states that assumption,
    whether the other file is analyzed or only listed in the compilation database.
    """
    print("=== Testing a const suggestion that takes the symbol of another file's overload ===")
    sources = {
        "api.hpp": "int f(int* p);\nint f(const int* p);\n",
        "a.cpp": '#include "api.hpp"\nint f(int* p)\n{\n    return *p;\n}\n',
        "b.cpp": '#include "api.hpp"\nint f(const int* p)\n{\n    return *p + 1;\n}\n',
    }
    claims = ("checked in the analyzed files only", "checked in every file of the compilation")
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_overload_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        database = root / "compile_commands.json"
        database.write_text(
            json.dumps(
                [
                    {
                        "directory": str(root),
                        "file": str(root / f),
                        "arguments": ["clang++", "-std=c++17", "-c", str(root / f)],
                    }
                    for f in ("a.cpp", "b.cpp")
                ]
            )
        )
        expected = (
            f"not checked with Clang: _Z1fPKi is also defined in {root / 'b.cpp'}; if "
            f"{root / 'a.cpp'} and {root / 'b.cpp'} are linked together, the two definitions "
            "collide"
        )
        for label, args in (
            ("both files analyzed", [str(root / "a.cpp"), str(root / "b.cpp")]),
            (
                "the other file only in the compilation database",
                [str(root / "a.cpp"), f"--compile-commands={database}"],
            ),
        ):
            run = _const_suggestion_run(args)
            texts = run[0].get("_Z1fPi", []) if run else []
            good = (
                len(texts) == 1
                and expected in texts[0]
                and not any(claim in texts[0] for claim in claims)
            )
            print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {texts}"))
            ok = ok and good
    print()
    return ok
```

Review Focus 3, a function defined in a header that two analyzed files include:

```python
def check_const_suggestion_inline_in_a_header() -> bool:
    """
    #217: a function defined in a header that two analyzed files include is defined in both: a C
    `static inline` function, one function in each file, and a C++ `inline` function, one function
    that both files define. Neither definition collides with the other: each suggestion is checked.
    """
    print("=== Testing const suggestions for functions defined in a shared header ===")
    sources = {
        "peek.h": "static inline int peek_c(int* p)\n{\n    return *p;\n}\n",
        "one.c": '#include "peek.h"\n'
        "int use_one(void)\n{\n    int v = 1;\n    return peek_c(&v);\n}\n",
        "two.c": '#include "peek.h"\n'
        "int use_two(void)\n{\n    int v = 2;\n    return peek_c(&v);\n}\n",
        "peek.hpp": "inline int peek_cpp(int* p)\n{\n    return *p;\n}\n",
        "three.cpp": '#include "peek.hpp"\n'
        "int use_three()\n{\n    int v = 3;\n    return peek_cpp(&v);\n}\n",
        "four.cpp": '#include "peek.hpp"\n'
        "int use_four()\n{\n    int v = 4;\n    return peek_cpp(&v);\n}\n",
    }
    checked = "checked in the analyzed files only"
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_inline_") as tmp:
        root = Path(tmp).resolve()
        for name, text in sources.items():
            (root / name).write_text(text)
        for label, files, function in (
            ("C, static inline", ["one.c", "two.c"], "peek_c"),
            ("C++, inline", ["three.cpp", "four.cpp"], "_Z8peek_cppPi"),
        ):
            run = _const_suggestion_run([str(root / f) for f in files])
            texts = run[0].get(function, []) if run else []
            # One suggestion for the definition in each file.
            good = len(texts) == 2 and all(checked in text for text in texts)
            print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": {texts}"))
            ok = ok and good
    print()
    return ok
```

D17, the two changed expectations:

```diff
--- a/run_test.py
+++ b/run_test.py
@@ -3,8 +3,9 @@
     #217: a suggested const type is reported only if every analyzed file still compiles with it,
     once every declaration the defining file sees is rewritten. Another analyzed file that stores
-    the function where a non-const parameter is expected, declares it itself, or converts its
-    address with an explicit cast, removes the suggestion; a homonym with internal linkage in
-    another file does not. A file that is not analyzed is not checked. The callback and cast files
-    sort before the definition, the redeclaration after it.
+    the function where a non-const parameter is expected, or converts its address with an explicit
+    cast, removes the suggestion. One that declares it itself, without the header, keeps it, not
+    checked: whether that matters depends on how the files are linked. A homonym with internal
+    linkage in another file does not change it. A file that is not analyzed is not checked. The
+    callback and cast files sort before the definition, the redeclaration after it.
     """
     return _check_cross_tu_rule(
@@ -17,5 +18,5 @@
             ("def and a caller", ["def.c", "caller.c"], ["read_value"]),
             ("def and a callback of the old type", ["def.c", "callback.c"], []),
-            ("def and a redeclaration", ["def.c", "redecl.c"], []),
+            ("def and a redeclaration", ["def.c", "redecl.c"], ["read_value"]),
             ("def and an explicit cast", ["def.c", "cast.c"], []),
             # Another file's homonym with internal linkage is another function.
```

```diff
--- a/run_test.py
+++ b/run_test.py
@@ -3,11 +3,12 @@
     #217: with a compilation database, a suggested const type is checked in every file the
     database lists that sees the change, analyzed or not, with its own compile command. A file
-    that is not analyzed and stores the function where a non-const parameter is expected, declares
-    it itself without the header, or converts its address with an explicit cast, removes the
-    suggestion, even listed after a file that cannot be checked. So does a file that takes the
-    address of a method with the old type: it sees the change through the header alone. A file
-    that sees the change but does not compile as it is, or cannot be preprocessed, keeps the
-    suggestion as not checked, without the claim of the check. A file that does not see the change
-    is not compiled.
+    that is not analyzed and stores the function where a non-const parameter is expected, or
+    converts its address with an explicit cast, removes the suggestion, even listed after a file
+    that cannot be checked. So does a file that takes the address of a method with the old type: it
+    sees the change through the header alone. A file that declares the function itself, without
+    the header, keeps the suggestion as not checked, and the reason names it and the link it
+    assumes. A file that sees the change but does not compile as it is, or cannot be preprocessed,
+    keeps the suggestion as not checked, without the claim of the check. A file that does not see
+    the change is not compiled.
     """
     print("=== Testing const suggestions checked in the compilation database ===")
@@ -87,5 +88,14 @@
             ),
             # Without the header: only the name of the function shows that it sees the change.
-            ("a redeclaration, not analyzed", ["def.c", "redecl.c"], "read_value", None),
+            (
+                "a redeclaration, not analyzed",
+                ["def.c", "redecl.c"],
+                "read_value",
+                "not checked with Clang: " + str(fixtures / f"{prefix}redecl.c")
+                + " declares read_value with its current type and does not see the rewrite: if "
+                + str(fixtures / f"{prefix}def.c") + " and " + str(fixtures / f"{prefix}redecl.c")
+                + " are linked together, that declaration no longer matches the rewritten "
+                "definition",
+            ),
             ("an explicit cast, not analyzed", ["def.c", "cast.c"], "read_value", None),
             ("a method alone", ["method.cpp"], method, checked),
```

Failure at `64ed37e`: t9 is "checked", in both runs; the redeclaration removes the suggestion, in both checks. Review Focus 3 passes at `64ed37e`: T v1 never sees a collision, so the test guards the exception of §5.10 ("a same entity defined in several files is no collision") against Task 1.6 itself. Its failure is shown on a mutant of this task's code (Step 4).

```text
check_const_suggestion_overload_defined_elsewhere: FAIL
=== Testing a const suggestion that takes the symbol of another file's overload ===
  ❌ both files analyzed: ["\t[ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'f(int*)' is never used to modify the pointed object\n\t\t ↳ current type: int *p\n\t\t ↳ suggested type: const int *p\ …
  ❌ the other file only in the compilation database: ["\t[ !Info! ] ConstParameterNotModified.Pointer: parameter 'p' in function 'f(int*)' is never used to modify the pointed object\n\t\t ↳ current type: int *p\n\t\t ↳ s …
```

```text
check_const_suggestion_inline_in_a_header: PASS
=== Testing const suggestions for functions defined in a shared header ===
  ✅ C, static inline
  ✅ C++, inline
```

```text
check_const_suggestion_cross_tu (Task 1.6 version): FAIL
=== Testing suggested const types checked in every analyzed file ===
  ✅ default, def alone: ['read_value']
  ✅ default, def and a caller: ['read_value']
  ✅ default, def and a callback of the old type: []
  ❌ default, def and a redeclaration: [], expected ['read_value']
  ✅ default, def and an explicit cast: []
  ✅ default, a static function and a static homonym: ['helper']
  ✅ default, an anonymous namespace's function and its homonym: ['_ZN12_GLOBAL__N_16helperEPi']
  ✅ smt-z3, def alone: ['read_value']
  ✅ smt-z3, def and a caller: ['read_value']
  ✅ smt-z3, def and a callback of the old type: []
  ❌ smt-z3, def and a redeclaration: [], expected ['read_value']
  ✅ smt-z3, def and an explicit cast: []
  ✅ smt-z3, a static function and a static homonym: ['helper']
  ✅ smt-z3, an anonymous namespace's function and its homonym: ['_ZN12_GLOBAL__N_16helperEPi']
```

```text
check_const_suggestion_compilation_database (Task 1.6 version): FAIL
=== Testing const suggestions checked in the compilation database ===
  ✅ a caller
  ✅ a callback of the old type, not analyzed
  ❌ a redeclaration, not analyzed: None
  ✅ an explicit cast, not analyzed
  ✅ a method alone
  ✅ a method's address of the old type, not analyzed
  ✅ a file that does not compile as it is
  ✅ a file that cannot be preprocessed
  ✅ a file that cannot be checked, then a callback
  ✅ a file that does not see the change
```

The static and anonymous-namespace homonyms of `check_const_suggestion_cross_tu` stay unchanged.

- [ ] **Step 1: Add the two checks and the D17 expectations; run them.** Expected: D4 and the redeclaration cases FAIL, as above; Review Focus 3 passes.
  Commits: `test(const-param): keep a rewrite that takes another file's symbol, unchecked`; `test(const-param): a function a shared header defines does not collide with itself`; `test(const-param): keep a suggestion that another file redeclares, unchecked`.
- [ ] **Step 2: Collisions (§5.10).** After the rewrite, each compilation looks up every symbol the entity takes (its variants after the rewrite) in its module: a definition whose declaration is not the entity's (Task 1.4's identity) is a collision, with the IR linkage of that definition. A declaration the entity's redeclarations gain, compared with the unit's initial compilation, is a merge (ruling 3). Once every unit is judged, and none refutes, `judgeCollisions` applies the table of §5.10:
  - local linkage (`internal`, `private`): no collision;
  - `external`: in the defining file, Clang reports a redefinition, refuted at step 4; in another file, indeterminate, `… the two definitions collide`;
  - `weak`, `extern_weak`, `common`: indeterminate, naming the definition the linker would use: the strong one;
  - `weak_odr`, `linkonce_odr`: indeterminate, `… only one of the two is kept …`;
  - a merge with a declaration: indeterminate, after any definition (ruling 1).
  A definition that is the entity's, such as an `inline` function of a rewritten header, is no collision.
  Run: `check_const_suggestion_overload_defined_elsewhere`, `check_const_suggestion_inline_in_a_header`. Expected: PASS.
  Commit: `feat(const-param): keep a rewrite that may collide with another file, unchecked`.
- [ ] **Step 3: Stale declarations (D17).** A unit that reads no rewritten file and declares the entity through the linker (Task 1.4, Step 3) makes the unit indeterminate, after steps 4 to 6: `<file> declares <function> with its current type and does not see the rewrite: …`. T v1's removal `declared in <file>:<line> with type <type>` goes.
  Run: `check_const_suggestion_cross_tu`, `check_const_suggestion_compilation_database`. Expected: PASS, but for the `cast.c` cases (Task 1.7).
  Commit: `fix(const-param): keep a suggestion that another file redeclares, unchecked`.
- [ ] **Step 4: Failure of Review Focus 3 on a mutant.** Count the entity's own definition as another entity's in Step 2's lookup; run `check_const_suggestion_inline_in_a_header`. Expected: FAIL (`C++, inline`: indeterminate, `… only one of the two is kept …`). Restore the code; expected: PASS. Record both outputs in `evidence/217/stage1/`.
- [ ] **Step 5: README (D3).** In "Const parameter suggestions checked with Clang", step 2: "A homonym with internal linkage (`static`, anonymous namespace) in another file is another function." becomes "A function with internal linkage (`static`, anonymous namespace) and a function of the same name in another file are two functions, whichever of them has internal linkage." The rest of the section is rewritten with the verdicts in Task 1.8.
  Commit: `docs(readme): a static function and its homonym elsewhere are two functions`.
- [ ] **Step 6: Task tests.** As Task 1.1, Step 6.

**Validation:** D4, Review Focus 3 and the D17 expectations pass; Review Focus 3 failed on the mutant.

### Task 1.7: explicit address casts (D15)

**Files:** `src/analysis/ConstSuggestionFacts.cpp`, `src/analysis/ConstSuggestionCheck.cpp`, `run_test.py`.

**Tests.** The two changed expectations, on top of Task 1.6's:

```diff
--- a/run_test.py
+++ b/run_test.py
@@ -3,9 +3,10 @@
     #217: a suggested const type is reported only if every analyzed file still compiles with it,
     once every declaration the defining file sees is rewritten. Another analyzed file that stores
-    the function where a non-const parameter is expected, or converts its address with an explicit
-    cast, removes the suggestion. One that declares it itself, without the header, keeps it, not
-    checked: whether that matters depends on how the files are linked. A homonym with internal
-    linkage in another file does not change it. A file that is not analyzed is not checked. The
-    callback and cast files sort before the definition, the redeclaration after it.
+    the function where a non-const parameter is expected removes the suggestion. One that declares
+    it itself, without the header, or converts its address with an explicit cast, keeps it, not
+    checked: whether that matters depends on how the files are linked, or on what the cast hides.
+    A homonym with internal linkage in another file does not change it. A file that is not
+    analyzed is not checked. The callback and cast files sort before the definition, the
+    redeclaration after it.
     """
     return _check_cross_tu_rule(
@@ -19,5 +20,5 @@
             ("def and a callback of the old type", ["def.c", "callback.c"], []),
             ("def and a redeclaration", ["def.c", "redecl.c"], ["read_value"]),
-            ("def and an explicit cast", ["def.c", "cast.c"], []),
+            ("def and an explicit cast", ["def.c", "cast.c"], ["read_value"]),
             # Another file's homonym with internal linkage is another function.
             (
```

```diff
--- a/run_test.py
+++ b/run_test.py
@@ -3,12 +3,12 @@
     #217: with a compilation database, a suggested const type is checked in every file the
     database lists that sees the change, analyzed or not, with its own compile command. A file
-    that is not analyzed and stores the function where a non-const parameter is expected, or
-    converts its address with an explicit cast, removes the suggestion, even listed after a file
-    that cannot be checked. So does a file that takes the address of a method with the old type: it
-    sees the change through the header alone. A file that declares the function itself, without
-    the header, keeps the suggestion as not checked, and the reason names it and the link it
-    assumes. A file that sees the change but does not compile as it is, or cannot be preprocessed,
-    keeps the suggestion as not checked, without the claim of the check. A file that does not see
-    the change is not compiled.
+    that is not analyzed and stores the function where a non-const parameter is expected removes
+    the suggestion, even listed after a file that cannot be checked. So does a file that takes the
+    address of a method with the old type: it sees the change through the header alone. A file
+    that declares the function itself, without the header, keeps the suggestion as not checked,
+    and the reason names it and the link it assumes; so does a file that converts the function's
+    address with an explicit cast, and the reason names the cast. A file that sees the change but
+    does not compile as it is, or cannot be preprocessed, keeps the suggestion as not checked,
+    without the claim of the check. A file that does not see the change is not compiled.
     """
     print("=== Testing const suggestions checked in the compilation database ===")
@@ -98,5 +98,11 @@
                 "definition",
             ),
-            ("an explicit cast, not analyzed", ["def.c", "cast.c"], "read_value", None),
+            (
+                "an explicit cast, not analyzed",
+                ["def.c", "cast.c"],
+                "read_value",
+                "not checked with Clang: " + str(fixtures / f"{prefix}cast.c")
+                + ":10 converts the address of read_value with an explicit cast",
+            ),
             ("a method alone", ["method.cpp"], method, checked),
             (
```

Failure at `64ed37e`: both remove the suggestion (`its address is converted by an explicit cast`). The cast cases of the two checks:

```text
check_const_suggestion_cross_tu: FAIL
=== Testing suggested const types checked in every analyzed file ===
  ❌ default, def and an explicit cast: [], expected ['read_value']
  ❌ smt-z3, def and an explicit cast: [], expected ['read_value']
```

```text
check_const_suggestion_compilation_database: FAIL
=== Testing const suggestions checked in the compilation database ===
  ❌ an explicit cast, not analyzed: None
```

- [ ] **Step 1: Change the two expectations and run both checks.** Expected: FAIL on the `cast.c` cases.
  Commit: `test(const-param): keep a suggestion whose address another file casts, unchecked`.
- [ ] **Step 2: The cast, named.** `VisitExplicitCastExpr` (T v1's, cast to `void` still excluded) records the cast's site in `UnitFacts::Entity::casts` instead of a problem that refutes. After steps 4 to 6, a recorded cast makes the unit indeterminate: `<file>:<line> converts the address of <function> with an explicit cast`, the first cast in source order.
  Run: both checks. Expected: PASS. `const-suggestion-void-cast.c`: unchanged, PASS.
  Commit: `fix(const-param): keep a suggestion whose address is cast, unchecked`.
- [ ] **Step 3: Task tests.** As Task 1.1, Step 6.

**Validation:** both checks pass with their new expectations.

### Task 1.8: trace and README

**Files:** `src/analysis/ConstSuggestionCheck.cpp`, `src/analysis/ConstSuggestionPlan.cpp`, `src/analysis/ConstSuggestionInternal.hpp`, `README.md`, `run_test.py`.

**Interfaces.** Adds `Candidate` and `SuggestionPlan::candidate` (Shared types): `current` and `type` printed with the parameter's name (`QualType::print` with the name as placeholder, `const int *const p` for `PointerConstOnly`), `alternative` from the rule (Texts).

**Tests.** A new check of the trace's fields, on one verified, one refuted and one indeterminate suggestion; `check_const_suggestion_removal_reason` unchanged:

```python
def check_const_suggestion_trace() -> bool:
    """
    #217: with --verbose, the trace gives each verdict's grounds after the line of its outcome: the
    suggestion (rule, function, the parameter's index among the source's parameters), the
    transformation (current type, candidate, alternative) and where it is written, each unit
    checked with its configuration and its verdict, then the verdict and its reason. Here one
    suggestion verified, one refuted and one not checked.
    """
    print("=== Testing the trace of const suggestion verdicts ===")
    source = (
        "int read_value(int* p)\n{\n    return *p;\n}\n"
        "static int read_slot(int* slot)\n{\n    return *slot;\n}\n"
        "int (*const stored)(int*) = read_slot;\n"
        "#define DECLARE(name) int name(int* q)\n"
        "DECLARE(read_macro)\n{\n    return *q;\n}\n"
    )
    with tempfile.TemporaryDirectory(prefix="ct_const_suggestion_trace_") as tmp:
        path = Path(tmp).resolve() / "verdicts.c"
        path.write_text(source)
        run = _const_suggestion_run([str(path)])
    if run is None:
        return fail_check("the analysis did not run")
    _, trace, _ = run
    file = re.escape(str(path))
    unit = rf"unit {file} \[[0-9a-f]+\]"
    rule = r"ConstParameterNotModified\.Pointer"
    expected = {
        "verified": [
            r"read_value parameter p: checked in 1 file\(s\)",
            rf"read_value parameter p, suggestion: {rule} on read_value, source index 0",
            r"read_value parameter p, transformation: int \*p becomes const int \*p "
            r"\(pointer to const\)",
            rf"read_value parameter p, rewritten at: {file}:1",
            rf"read_value parameter p, {unit}: verified",
            r"read_value parameter p, verdict: verified",
        ],
        "refuted": [
            rf"read_slot parameter slot: removed, does not compile: {file}:9:",
            rf"read_slot parameter slot, suggestion: {rule} on read_slot, source index 0",
            r"read_slot parameter slot, transformation: int \*slot becomes const int \*slot "
            r"\(pointer to const\)",
            rf"read_slot parameter slot, rewritten at: {file}:5",
            rf"read_slot parameter slot, {unit}: refuted: does not compile: {file}:9:",
            rf"read_slot parameter slot, verdict: refuted: does not compile: {file}:9:",
        ],
        "not checked": [
            r"read_macro parameter q: not checked, the declaration is written by a macro "
            r"definition",
            rf"read_macro parameter q, suggestion: {rule} on read_macro, source index 0",
            r"read_macro parameter q, transformation: int \*q becomes const int \*q "
            r"\(pointer to const\)",
            r"read_macro parameter q, verdict: indeterminate: the declaration is written by a "
            r"macro definition",
        ],
    }
    ok = True
    for label, patterns in expected.items():
        missing = [p for p in patterns if not any(re.match(p, line) for line in trace)]
        good = not missing
        print(f"  {'✅' if good else '❌'} {label}" + ("" if good else f": missing {missing}"))
        ok = ok and good
    if not ok:
        print("     trace:\n" + "\n".join(f"       {line}" for line in trace))
    print()
    return ok
```

Failure at `64ed37e`: only T v1's line per suggestion.

```text
check_const_suggestion_trace: FAIL
=== Testing the trace of const suggestion verdicts ===
  ❌ verified: missing ['read_value parameter p, suggestion: ConstParameterNotModified\\.Pointer on read_value, source index 0', 'read_value parameter p, transformation: int \\*p becomes const int \\*p \\(pointer to const …
  ❌ refuted: missing ['read_slot parameter slot, suggestion: ConstParameterNotModified\\.Pointer on read_slot, source index 0', 'read_slot parameter slot, transformation: int \\*slot becomes const int \\*slot \\(pointer  …
  ❌ not checked: missing ['read_macro parameter q, suggestion: ConstParameterNotModified\\.Pointer on read_macro, source index 0', 'read_macro parameter q, transformation: int \\*q becomes const int \\*q \\(pointer to co …
```

- [ ] **Step 1: Add the check and run it.** Expected: FAIL, as above.
  Commit: `test(const-param): give each verdict's grounds in the trace`.
- [ ] **Step 2: The trace (§4).** After T v1's line, `checkConstSuggestions` logs the `suggestion`, `transformation`, `rewritten at` and `verdict` lines (Texts), around Task 1.1's `unit` lines. A suggestion not planned has no `rewritten at` and no `unit` line.
  Run: `check_const_suggestion_trace`, `check_const_suggestion_removal_reason`. Expected: PASS.
  Commit: `feat(const-param): trace the suggestion, its rewrite, its units and its verdict`.
- [ ] **Step 3: README.** "Const parameter suggestions checked with Clang" describes:
  - the three verdicts (§3): verified, with the "checked" line; refuted, removed, on an error or a tracked warning introduced, or a changed call, override or constant condition; indeterminate, kept with `not checked with Clang: <reason>`;
  - steps 1 to 3 as T v2 runs them: the plan and its scope, the units that see the change, the judgement of §5.8;
  - the limits of §10: collisions between files and the ODR not decided, one configuration per file, a change of behavior that R, O and K do not cover kept as not checked (`signature_size`), a cost to measure (stage 3).
  The experimental notice keeps its sense; the sentence "A difference its normalizations do not cover removes a suggestion that is valid" becomes "A difference that the facts do not explain keeps the suggestion as not checked".
  Commit: `docs(readme): describe the three verdicts of the const suggestion check`.
- [ ] **Step 4: Task tests.** As Task 1.1, Step 6.

**Validation:** both checks pass.

**Validation of stage 1:** the catalogue tests D1 (T), D2, D3, D4, D5, D7, D11, D14, D15, D16, D17, Review Focus 2 to 4, and the tests that rulings 6, 11 and 13 add, pass; every T v1 test passes, except the changed expectations of D14, D15, D16 and D17; the suite without and with the option, the unit tests, on macOS and Linux; the review cases t1 to t15 give the verdicts of §6 for the stage-1 IDs; CI green and code scanning triaged.

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
**Tests:** D12, changed expectations of the two `ReferenceRvaluePreferValue` blocks of `const-suggestion-deduction-kept.cpp`, the only T fixture with that rule; D13, a new fixture that enables T (t15: `int* const p` only read, displayed and verified as `const int *const p`), presented with its failure evidence; no T v1 fixture covers `PointerConstOnly`, and the existing test without the option stays unchanged.

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
