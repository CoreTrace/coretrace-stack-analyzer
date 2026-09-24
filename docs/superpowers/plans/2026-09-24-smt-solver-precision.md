# SMT Solver Precision — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `--smt=on --smt-backend=z3` remove false positives without ever hiding a true positive, in three pull requests: Z3 really exercised in CI (A0), the solver asked for every candidate with wrapping semantics (A1), path conditions over a MemorySSA memory model (A2).

**Architecture:** The rules keep their current detection and only consult `SmtConstraintEvaluator`; an `unsat` answer is still the only thing that suppresses a diagnostic. A0 fixes the build and CI so that Z3 is present and its absence is loud. A1 makes the evaluator encode lazily, removes the "known range required" preconditions and the `nsw`/`nuw` assumptions of the encoder. A2 gives `FunctionFacts` a MemorySSA, encodes loads through their clobbering access, and adds to each query the reachability condition of the query point over the CFG without back edges.

**Tech Stack:** C++20, LLVM 20 (MemorySSA, BasicAA, DominatorTree, `FindFunctionBackedges`, `ReversePostOrderTraversal`), Z3 C++ API, CMake with pkg-config, Python 3 (`run_test.py`), GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md`

## Global Constraints

- TDD: write the test, run it and see it fail, implement, run it and see it pass (spec, user rule).
- Existing tests and fixtures are not modified without the user's explicit agreement: when one fails, stop, report it, and wait (spec §7).
- `--smt` stays off by default. With SMT off, diagnostics are identical to `main` and analysis time does not change beyond measurement noise (spec §2, §7).
- Only an `unsat` answer suppresses; `sat`, `unknown`, `timeout` and `error` keep the diagnostic (spec §2).
- Budgets stay the existing defaults: 50 ms per query (`--smt-timeout-ms`), 10 000 nodes per query (`--smt-budget-nodes`) (spec §2).
- Commits: the repository's git identity (Hugo <hugo.payet@epitech.eu>), no `Co-Authored-By` trailer, no mention of Claude; conventional-commit subject (`feat|fix|chore|docs|refactor|perf|ci|build|style|revert|test`), at most 84 characters; body line `Refs #<issue>`.
- Never stage the user's untracked files: `.DS_Store`, `TODO.md`, `docs/architecture/inter-tu-analysis.md`, `pr-90-review.md`, `test-cedric/`. Stage files by name, never with `git add -A` or `git add .`.
- Format every modified C/C++ file with `/opt/homebrew/opt/llvm@20/bin/clang-format -i <files>` (clang-format 20, as CI).
- Build: `cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8`. Unit tests: `./build/stack_usage_analyzer_unit_tests . && ./build/ownership_engine_unit_tests`. Fixtures: `python3 run_test.py --jobs 8 --no-cache`.
- Push over HTTPS: `GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=credential.helper GIT_CONFIG_VALUE_0='!gh auth git-credential' git push -q https://github.com/CoreTrace/coretrace-stack-analyzer.git <branch>`.
- One issue and one PR per part, in English, PR against `main`. The user merges: after opening a PR, stop until the user reports the merge, then start the next part from an updated `main`.
- The query trace used for measurements is never committed (spec §7); it only lives in the measurement worktrees of Procedure M.

## Review Focus

Inputs the spec implies but no spec-listed test exercises, most likely first. Each one has its test in the owning task.

1. A store of a narrower type to the same slot as a wider load (type punning, `*(char*)&x = 5; return x - 1;`) must not be forwarded to the load — test in Task A2.3.
2. `volatile` loads must never share a symbol, even from the same address with no store between them — test in Task A2.3.
3. `switch` terminators: several cases reaching the same block, and the default edge — test in Task A2.4.
4. A query whose instruction is in the entry block has no dominator: no path condition, no crash — test in Task A2.4.
5. Backend names are case-insensitive, as `--smt-backend` accepts them: `--smt-backend=Interval` must not warn — test in Task A0.2.

## Tooling (local, never committed)

All paths below use `SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad`. Shell state does not persist between commands: define `SCRATCH` in each command that uses it. Commands run from the repository root unless stated.

`$SCRATCH/smtimprove/check_fixtures.py` runs the given fixtures through `run_test.check_file` without the output cache:

```python
#!/usr/bin/env python3
"""Run the given fixtures through run_test.check_file with the output cache disabled."""
import pathlib
import sys

import run_test

run_test.RUN_CONFIG.cache_enabled = False
ok = True
for path in sys.argv[1:]:
    passed, _, _, report = run_test.check_file(pathlib.Path(path))
    print(report, end="")
    ok = ok and passed
sys.exit(0 if ok else 1)
```

Usage: `PYTHONPATH=. python3 "$SCRATCH/smtimprove/check_fixtures.py" <fixture>...`

`$SCRATCH/smtimprove/insert_trace.py` inserts the #103 query trace into copies of `SmtRefinement.hpp`:

```python
#!/usr/bin/env python3
"""Insert the throwaway #103 query trace into SmtRefinement.hpp copies (never committed)."""
import sys

TRACE = """            // SPIKE #103 (throwaway): one line per solver query.
            if (std::getenv("CTRACE_SMT_TRACE"))
            {
                static const char* kStatus[] = {"sat", "unsat", "unknown", "timeout", "error"};
                std::fprintf(stderr, "[smt-trace] rule=%s status=%s\\n", ruleId_.c_str(),
                             kStatus[static_cast<unsigned>(decision.status) % 5]);
            }
"""

for path in sys.argv[1:]:
    out = []
    for line in open(path).read().splitlines(keepends=True):
        out.append(line)
        if line.strip() == "#include <cstdint>":
            out.append("#include <cstdio>\n#include <cstdlib>\n")
        if "orchestrator_->solve(query);" in line:
            out.append(TRACE)
    text = "".join(out)
    assert "[smt-trace]" in text, f"solver call not found in {path}"
    open(path, "w").write(text)
```

`$SCRATCH/smtimprove/compare.py` compares two analyzers on the same units:

```python
#!/usr/bin/env python3
"""Before/after diagnostics for the SMT regression reports (local tool, never committed).

usage: compare.py BASELINE CANDIDATE JOBS EXTRA_JSON UNITS_JSON

Runs both analyzers on every unit, with SMT off and with Z3 on the five SMT rules, and prints
per configuration: wall time, failed runs, the diagnostics CANDIDATE removes and adds (with the
source line, for labelling), and the [smt-trace] query counts by rule and status.
"""
import collections
import json
import linecache
import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

RULES = "recursion,integer-overflow,size-minus-k,stack-buffer,oob-read"
CONFIGS = {
    "off": [],
    "z3": ["--smt=on", "--smt-backend=z3", "--smt-mode=single", f"--smt-rules={RULES}"],
}
TIMEOUT_S = 600


def key(d):
    loc = d.get("location", {})
    message = d.get("details", {}).get("message", "").strip()
    headline = message.splitlines()[0].strip() if message else ""
    return (d.get("ruleId"), loc.get("file"), loc.get("function"), loc.get("startLine"),
            loc.get("startColumn"), headline)


def run(analyzer, extra, cfg, unit):
    env = dict(os.environ, CTRACE_SMT_TRACE="1")
    try:
        p = subprocess.run([analyzer, "--format=json", *extra, *CONFIGS[cfg], *unit],
                           capture_output=True, text=True, timeout=TIMEOUT_S, env=env)
        diags = json.loads(p.stdout).get("diagnostics", [])
    except (subprocess.TimeoutExpired, json.JSONDecodeError):
        return None, collections.Counter()
    trace = collections.Counter(line.split(" ", 1)[1] for line in p.stderr.splitlines()
                                if line.startswith("[smt-trace] "))
    return {key(d) for d in diags}, trace


def measure(analyzer, extra, cfg, units, jobs):
    start = time.time()
    with ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(lambda unit: run(analyzer, extra, cfg, unit), units))
    keys, failed, trace = set(), [], collections.Counter()
    for unit, (unit_keys, unit_trace) in zip(units, results):
        if unit_keys is None:
            failed.append(os.path.basename(unit[-1]))
        else:
            keys |= unit_keys
            trace += unit_trace
    return keys, failed, trace, time.time() - start


def show(label, diags):
    print(f"  {label}: {len(diags)}")
    for rule, file, function, line, column, headline in sorted(diags, key=str):
        source = linecache.getline(file, line).strip() if file and line else ""
        print(f"    {rule} {os.path.basename(file or '')}:{line}:{column} {function}"
              f" | {headline} | {source}")


def main():
    baseline, candidate, jobs = sys.argv[1], sys.argv[2], int(sys.argv[3])
    extra, units = json.loads(sys.argv[4]), json.loads(sys.argv[5])
    for cfg in CONFIGS:
        base_keys, base_failed, base_trace, base_time = measure(baseline, extra, cfg, units, jobs)
        cand_keys, cand_failed, cand_trace, cand_time = measure(candidate, extra, cfg, units, jobs)
        print(f"=== {cfg}: {len(units)} unit(s) ===")
        print(f"  time: baseline {base_time:.1f}s, candidate {cand_time:.1f}s")
        print(f"  failed runs: baseline {base_failed}, candidate {cand_failed}")
        print(f"  diagnostics: baseline {len(base_keys)}, candidate {len(cand_keys)}")
        show("removed by candidate", base_keys - cand_keys)
        show("added by candidate", cand_keys - base_keys)
        for label, trace in (("baseline", base_trace), ("candidate", cand_trace)):
            if trace:
                print(f"  queries ({label}): "
                      + ", ".join(f"{k}={v}" for k, v in sorted(trace.items())))


if __name__ == "__main__":
    main()
```

### Procedure M — before/after measurement

Run for a part's regression report, on the part's branch, with everything committed.

1. Build instrumented analyzers in two worktrees, the baseline on `origin/main` and the candidate on the branch head:

```bash
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
git fetch -q origin
git worktree add --detach "$SCRATCH/wt-base" origin/main
git worktree add --detach "$SCRATCH/wt-cand" HEAD
python3 "$SCRATCH/smtimprove/insert_trace.py" "$SCRATCH/wt-base/include/analysis/smt/SmtRefinement.hpp" "$SCRATCH/wt-cand/include/analysis/smt/SmtRefinement.hpp"
for wt in wt-base wt-cand; do
  cmake -S "$SCRATCH/$wt" -B "$SCRATCH/$wt/build" -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang >/dev/null
  cmake --build "$SCRATCH/$wt/build" --target stack_usage_analyzer -j 8 >/dev/null || echo "BUILD FAILED: $wt"
done
```

Expected: no `BUILD FAILED` line.

2. Run the four corpora (each command takes minutes; run them in the background and wait for completion):

```bash
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
B="$SCRATCH/wt-base/build/stack_usage_analyzer"; C="$SCRATCH/wt-cand/build/stack_usage_analyzer"; R="$SCRATCH/smtimprove/report-$(git rev-parse --short HEAD)"; mkdir -p "$R"
FIX=$(find "$PWD/test" -name '*.c' -not -path '*/test/unit/*' | sort | python3 -c "import sys,json; print(json.dumps([[l.strip()] for l in sys.stdin]))")
LUA=$(ls "$SCRATCH"/smt103/lua/*.c | grep -vE '/(lvm|onelua)\.c$' | python3 -c "import sys,json; print(json.dumps([[l.strip()] for l in sys.stdin]))")
ZLIB=$(ls "$SCRATCH"/smt103/zlib/*.c | grep -vE '/(deflate|infback|inflate|inffast)\.c$' | python3 -c "import sys,json; print(json.dumps([[l.strip()] for l in sys.stdin]))")
python3 "$SCRATCH/smtimprove/compare.py" "$B" "$C" 8 '[]' "$FIX" > "$R/fixtures.txt"
python3 "$SCRATCH/smtimprove/compare.py" "$B" "$C" 8 "[\"-I$SCRATCH/smt103/lua\"]" "$LUA" > "$R/lua.txt"
python3 "$SCRATCH/smtimprove/compare.py" "$B" "$C" 8 "[\"-I$SCRATCH/smt103/zlib\"]" "$ZLIB" > "$R/zlib.txt"
python3 "$SCRATCH/smtimprove/compare.py" "$B" "$C" 8 "[\"--compile-commands=$PWD/build/compile_commands.json\"]" "$(cat "$SCRATCH/smt103/self_units.json")" > "$R/self.txt"
```

3. Label every line under `removed by candidate` and `added by candidate` as a true or false positive, with a one-line justification read from the source. A removed true positive blocks the PR: stop and report it.

4. Remove the worktrees:

```bash
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
git worktree remove --force "$SCRATCH/wt-base"; git worktree remove --force "$SCRATCH/wt-cand"
```

The regression report of the PR gives, per corpus: diagnostics removed and added per rule with their labels, queries per rule and status, time for both configurations, failed runs.

## File Structure

| File | Part | Responsibility |
|---|---|---|
| `CMakeLists.txt` | A0 | Z3 detection falls back to pkg-config; links `PkgConfig::Z3`. |
| `.github/workflows/ci.yml` | A0 | Installs Z3 on Linux and macOS. |
| `include/analysis/smt/SolverOrchestrator.hpp`, `src/analysis/smt/SolverOrchestrator.cpp` | A0 | `isSmtBackendAvailable(name)`, single source of backend availability. |
| `main.cpp` | A0 | Warns once when `--smt=on` uses a backend missing from the build. |
| `run_test.py` | A0, A1 | `check_smt_unavailable_backend_warning`; `[default]`/`[smt-z3]` expectation prefixes. |
| `include/analysis/smt/SmtRefinement.hpp` | A1, A2 | Lazy `evaluateQuery`; `queryPoint` helper. |
| `src/analysis/smt/SmtEncoding.cpp`, `include/analysis/smt/SmtEncoding.hpp` | A1, A2 | Wrapping semantics; `QueryPoint`; memory-aware loads; reachability condition. |
| `src/analysis/{IntegerOverflowAnalysis,SizeMinusKWrites,OOBReadAnalysis,StackBufferAnalysis,StackComputation}.cpp` | A1, A2 | Lambdas for lazy encoding; preconditions removed; `FunctionFacts` passed to queries. |
| `include/analysis/FunctionFacts.hpp`, `src/analysis/FunctionFacts.cpp` | A2 | `clobberingAccess(load)` over lazily built BasicAA and MemorySSA. |
| `test/unit/analyzer_module_unit_tests.cpp`, `test/unit/smt_path_input.c` (new) | A1, A2 | Evaluator laziness; clobbers; encoder memory model and path condition. |
| `test/integer-overflow/smt-*.c`, `test/bound-storage/smt-path-*.c` (new) | A1, A2 | End-to-end fixtures. |
| `docs/architecture/smt-solver-integration.md` | A2 | Implementation status. |

---

## Part A0 — Z3 really exercised in CI

Branch `ci/smt-z3-backend` (already created from `origin/main`; it holds the spec commit).

### Task 1 (A0.1): Issue and #103 follow-up

**Files:** none.

- [ ] **Step 1: Create the A0 issue**

```bash
gh issue create --title "Build and exercise the Z3 SMT backend in CI" --body "$(cat <<'EOF'
## Problem
CI never builds the Z3 backend: both jobs print `SMT Z3 backend disabled (Z3 not found)` (run 35666706010). `--smt-backend=z3` then silently answers `Unknown`, so the dedicated smt-z3 fixture pass of `run_test.py` checks nothing. On Ubuntu 24.04, `libz3-dev` ships `z3.pc` but no `Z3Config.cmake`, which is the only thing the CMake detection looks for.

## Change
- CMake: fall back to pkg-config when `find_package(Z3 CONFIG)` fails.
- CI: install Z3 on Linux and macOS.
- Analyzer: one stderr warning when `--smt=on` uses a backend that is not compiled in.
- `run_test.py`: `check_smt_unavailable_backend_warning`, which also fails the run when this analyzer has no Z3.

Nothing changes without `--smt=on`.

Design: docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md (§3). Part of the follow-up to #103.
EOF
)"
```

Record the issue number as `A0_ISSUE`.

- [ ] **Step 2: Comment on #103**

```bash
gh issue comment 103 --body "$(cat <<'EOF'
## Result
With `--smt=on --smt-backend=z3` on the five SMT rules, no diagnostic changes:

| Corpus | Diagnostics | Queries | Changed |
|---|---|---|---|
| Fixtures (309 files) | 528 | 55, of which 3 `unsat` | 0 |
| Lua 5.4 (32 files) | 310 | 78, all `sat` | 0 |
| zlib (8 usable files of 15) | 28 | 3, all `sat` | 0 |

The solver can only confirm what the range check already found: queries carry the same interval and no branch condition, every `load` is a free symbol at `-O0`, integer-overflow skips the solver when no two-sided range is known, and CI never builds Z3.

## Follow-up
Rather than removing the layer, three changes make it effective (design: `docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md`): Z3 in CI (#A0_ISSUE), querying without ranges with wrapping semantics, then path conditions over MemorySSA.
EOF
)"
```

Replace `#A0_ISSUE` with the number from Step 1 before running.

### Task 2 (A0.2): Warn when `--smt=on` uses a backend missing from the build

**Files:**
- Modify: `include/analysis/smt/SolverOrchestrator.hpp`
- Modify: `src/analysis/smt/SolverOrchestrator.cpp:71-88`
- Modify: `main.cpp:1-12` (includes), `main.cpp:191-230` (`main`)
- Modify: `run_test.py` (new check, registered in `parallel_checks`)

**Interfaces:**
- Produces: `bool ctrace::stack::analysis::smt::isSmtBackendAvailable(std::string_view name)`; `check_smt_unavailable_backend_warning() -> bool` in `run_test.py`; the stderr fragment `is not available in this build`.

- [ ] **Step 1: Build the branch as it is**

Run: `cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8`
Expected: build succeeds (the tree was last built from another branch).

- [ ] **Step 2: Write the failing check**

In `run_test.py`, add after `check_help_flags`:

```python
_SMT_BACKEND_UNAVAILABLE = "is not available in this build"


def check_smt_unavailable_backend_warning() -> bool:
    """
    `--smt=on` must say on stderr when a backend it uses is not compiled in. This run also
    needs Z3: without it the dedicated smt-z3 fixture pass silently checks nothing.
    """
    print("=== Testing SMT backend availability ===")
    if _runner_has_explicit_smt_args():
        print("  [info] skipped (runner already has --smt args)")
        print()
        return True

    fixture = str(fixture_path_with_fallback("integer-overflow/nsw-flag-must-not-discharge-itself.c"))
    # (analyzer arguments, backend that must be reported once, or None for no warning)
    cases = [
        (["--smt=on", "--smt-backend=z3"], None),
        (["--smt=on", "--smt-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-backend=Interval"], None),
        (["--smt=on", "--smt-mode=portfolio", "--smt-secondary-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-mode=single", "--smt-secondary-backend=cvc5"], None),
        (["--smt-backend=cvc5", "--smt=off"], None),
    ]
    ok = True
    for args, backend in cases:
        result = run_analyzer([*args, fixture])
        stderr = result.stderr or ""
        warnings = stderr.count(_SMT_BACKEND_UNAVAILABLE)
        label = " ".join(args)
        if _SMT_BACKEND_UNAVAILABLE in (result.stdout or ""):
            print(f"  ❌ {label}: the warning must go to stderr, not stdout")
            ok = False
        elif backend is None and warnings:
            print(f"  ❌ {label}: unexpected warning")
            if "SMT backend 'z3'" in stderr:
                print("     this analyzer has no Z3: install it (libz3-dev or brew z3) and rebuild")
            ok = False
        elif backend is not None and (warnings != 1 or f"SMT backend '{backend}'" not in stderr):
            print(f"  ❌ {label}: expected one warning for '{backend}', got {warnings}")
            ok = False
    if ok:
        print("  ✅ SMT backend availability OK")
    print()
    return ok
```

Register it in `main()`'s `parallel_checks` list, right after `check_help_flags,`:

```python
        check_help_flags,
        check_smt_unavailable_backend_warning,
```

- [ ] **Step 3: Run it to see it fail**

Run: `python3 -c "import run_test; run_test.RUN_CONFIG.cache_enabled = False; raise SystemExit(0 if run_test.check_smt_unavailable_backend_warning() else 1)"`
Expected: FAIL, `--smt=on --smt-backend=cvc5: expected one warning for 'cvc5', got 0` and the same for the portfolio case.

- [ ] **Step 4: Add `isSmtBackendAvailable`**

In `include/analysis/smt/SolverOrchestrator.hpp`, add `#include <string_view>` and, after the `SolverOrchestrator` class:

```cpp
    /// @brief Whether @p name designates a backend compiled into this build.
    ///
    /// Names match case-insensitively, as `--smt-backend` accepts them; an empty name selects
    /// the interval backend. A missing backend answers every query Unknown.
    [[nodiscard]] bool isSmtBackendAvailable(std::string_view name);
```

In `src/analysis/smt/SolverOrchestrator.cpp`, replace `createBackend` (lines 71-88) with:

```cpp
        static std::shared_ptr<ISmtBackend> createBackend(std::string_view name)
        {
            if (!isSmtBackendAvailable(name))
                return std::make_shared<UnavailableExternalBackend>(std::string(name));
#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
            if (toLowerAscii(name) == "z3")
                return std::make_shared<Z3Backend>();
#endif
            return std::make_shared<IntervalBackend>();
        }
```

and add, after the closing brace of the anonymous namespace:

```cpp
    bool isSmtBackendAvailable(std::string_view name)
    {
        const std::string lowered = toLowerAscii(name);
        if (lowered.empty() || lowered == "interval")
            return true;
#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
        if (lowered == "z3")
            return true;
#endif
        return false;
    }
```

- [ ] **Step 5: Warn from `main`**

In `main.cpp`, add `#include "analysis/smt/SolverOrchestrator.hpp"` after `#include "cli/ArgParser.hpp"`, and `#include <vector>` with the standard includes. Add before `int main`:

```cpp
// A backend missing from this build answers every query Unknown, which silently turns
// --smt=on into a no-op; say so once, before any analysis.
static void warnAboutUnavailableSmtBackends(const ctrace::stack::AnalysisConfig& cfg)
{
    using ctrace::stack::analysis::smt::SolverMode;
    if (!cfg.smtEnabled)
        return;

    std::vector<std::string> used{cfg.smtBackend};
    if (cfg.smtMode != SolverMode::Single && !cfg.smtSecondaryBackend.empty() &&
        cfg.smtSecondaryBackend != cfg.smtBackend)
    {
        used.push_back(cfg.smtSecondaryBackend);
    }

    for (const std::string& backend : used)
    {
        if (!ctrace::stack::analysis::smt::isSmtBackendAvailable(backend))
        {
            coretrace::log(coretrace::Level::Warn,
                           "SMT backend '{}' is not available in this build; its queries are "
                           "inconclusive\n",
                           backend);
        }
    }
}
```

In `main`, right after `if (parseResult.parsed.printEffectiveConfig) printEffectiveConfig(parseResult.parsed);`:

```cpp
    warnAboutUnavailableSmtBackends(parseResult.parsed.config);
```

- [ ] **Step 6: Build, format, run the check**

Run:
```bash
/opt/homebrew/opt/llvm@20/bin/clang-format -i main.cpp include/analysis/smt/SolverOrchestrator.hpp src/analysis/smt/SolverOrchestrator.cpp
cmake --build build --target stack_usage_analyzer -j 8
python3 -c "import run_test; run_test.RUN_CONFIG.cache_enabled = False; raise SystemExit(0 if run_test.check_smt_unavailable_backend_warning() else 1)"
```
Expected: `✅ SMT backend availability OK`.

- [ ] **Step 7: Full suite and commit**

Run the build, unit tests and `python3 run_test.py --jobs 8 --no-cache` (Global Constraints). Expected: all green.

```bash
git add main.cpp include/analysis/smt/SolverOrchestrator.hpp src/analysis/smt/SolverOrchestrator.cpp run_test.py
git commit -m "feat(smt): warn when --smt=on uses a backend missing from the build" -m "Refs #A0_ISSUE"
```

### Task 3 (A0.3): Detect Z3 through pkg-config and install it in CI

**Files:**
- Modify: `CMakeLists.txt:114-124` (detection), `CMakeLists.txt:143-155` (linking)
- Modify: `.github/workflows/ci.yml:41-43` (Linux packages), `.github/workflows/ci.yml:57` (macOS packages)

**Interfaces:**
- Consumes: the `SMT Z3 backend enabled` configure message.
- Produces: `CTRACE_STACK_HAVE_Z3_BACKEND=ON` wherever `z3.pc` is visible.

- [ ] **Step 1: Show the fallback is missing**

Run:
```bash
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
cmake -S . -B "$SCRATCH/build-pc" -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON | grep "SMT Z3"
```
Expected: `SMT Z3 backend disabled (Z3 not found)`, although `/opt/homebrew/lib/pkgconfig/z3.pc` exists (this mimics Ubuntu, which only has `z3.pc`).

- [ ] **Step 2: Add the fallback**

In `CMakeLists.txt`, replace `find_package(Z3 QUIET CONFIG)` (line 116) with:

```cmake
    find_package(Z3 QUIET CONFIG)
    if(NOT Z3_FOUND)
        # Debian and Ubuntu ship z3.pc but no Z3Config.cmake.
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(Z3 QUIET IMPORTED_TARGET z3)
        endif()
    endif()
```

In the linking block, insert before `elseif(DEFINED Z3_LIBRARIES)`:

```cmake
    elseif(TARGET PkgConfig::Z3)
        target_link_libraries(stack_usage_analyzer_lib PUBLIC PkgConfig::Z3)
```

- [ ] **Step 3: Verify the fallback configures, links and answers**

Run:
```bash
SCRATCH=/private/tmp/claude-501/-Users-hugopayet-Desktop-CLaude-coretrace-stack-analyzer/1d774ad5-dc2c-46f4-8bd8-f6a1b832078d/scratchpad
cmake -S . -B "$SCRATCH/build-pc" -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang -DCMAKE_DISABLE_FIND_PACKAGE_Z3=ON | grep "SMT Z3"
cmake --build "$SCRATCH/build-pc" --target stack_usage_analyzer -j 8 >/dev/null && otool -L "$SCRATCH/build-pc/stack_usage_analyzer" | grep -c z3
"$SCRATCH/build-pc/stack_usage_analyzer" --smt=on --smt-backend=z3 test/integer-overflow/nsw-flag-must-not-discharge-itself.c 2>&1 | grep -c "not available"
rm -rf "$SCRATCH/build-pc"
```
Expected: `SMT Z3 backend enabled`, then `1` (libz3 linked), then `0` (no warning).

- [ ] **Step 4: Install Z3 in CI**

In `.github/workflows/ci.yml`, Linux step:

```yaml
        sudo apt-get install -y build-essential cmake python3 \
          ninja-build ccache lld libz3-dev pkg-config
```

macOS step:

```yaml
        brew install cmake python llvm@20 ninja ccache z3
```

- [ ] **Step 5: Local configure unchanged, commit**

Run: `cmake -S . -B build | grep "SMT Z3"`
Expected: `SMT Z3 backend enabled` (Homebrew's `Z3Config.cmake` path is still taken first).

```bash
git add CMakeLists.txt .github/workflows/ci.yml
git commit -m "ci(smt): build the Z3 backend on Linux and macOS" -m "Refs #A0_ISSUE"
```

### Task 4 (A0.4): Validate, report, open the PR

**Files:** none.

- [ ] **Step 1: Full validation**

Run the build, unit tests and `python3 run_test.py --jobs 8 --no-cache`; check formatting of tracked C/C++ files changed on the branch:

```bash
git diff --name-only origin/main -- '*.cpp' '*.hpp' | xargs /opt/homebrew/opt/llvm@20/bin/clang-format --dry-run --Werror
```
Expected: all green, no formatting output.

- [ ] **Step 2: Push and open the PR**

Push the branch (Global Constraints), then:

```bash
gh pr create --base main --head ci/smt-z3-backend --title "ci(smt): build and exercise the Z3 backend" --body "$(cat <<'EOF'
Closes #A0_ISSUE

Design: [docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md](docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md) · Plan: [docs/superpowers/plans/2026-09-24-smt-solver-precision.md](docs/superpowers/plans/2026-09-24-smt-solver-precision.md)

## What
- CMake falls back to pkg-config when `Z3Config.cmake` is missing (Ubuntu's `libz3-dev` only ships `z3.pc`).
- CI installs Z3 on both jobs, so the smt-z3 fixture pass really runs Z3.
- `--smt=on` with a backend missing from the build prints one stderr warning (it used to answer `Unknown` silently).
- `run_test.py` gains `check_smt_unavailable_backend_warning`, which also fails the run when the analyzer has no Z3.

## Regression report
<test counts, CI "SMT Z3 backend enabled" on both jobs, SMT-off behaviour unchanged by construction>
EOF
)"
```

Fill the `Regression report` section with the actual numbers before creating the PR.

- [ ] **Step 3: Check CI**

Run: `gh run list --branch ci/smt-z3-backend --limit 1`, then on the run: `gh run view <id> --log | grep "SMT Z3 backend"`.
Expected: `SMT Z3 backend enabled` on both jobs, and a green run.

- [ ] **Step 4: Stop**

Report to the user and wait for the merge before Part A1.

---

## Part A1 — Query without ranges, with wrapping semantics

### Task 5 (A1.1): Issue and branch

- [ ] **Step 1: Create the issue**

```bash
gh issue create --title "SMT: ask the solver even without a known range, with wrapping semantics" --body "$(cat <<'EOF'
## Problem
The SMT rules skip the solver unless a range is already known, so bit-width facts are never used: `return 1 + s->n;` (`unsigned char n`) and `memcpy(d, s, l * sizeof(char))` stay reported although neither can overflow (8 such false positives in Lua 5.4). The encoder also asserts `nsw`/`nuw` on every operand it encodes; once the solver is asked more often, that assumption would hide `return (a + 1) - 1;`, whose only report is on the `sub`.

## Change
- `SmtConstraintEvaluator` builds a query only when SMT is on for its rule.
- The eight "known range required" preconditions go.
- Operations are encoded with the wrapping semantics that -O0 code executes.
- `run_test.py` accepts `[default]` / `[smt-z3]` expectation prefixes.

Design: docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md (§4).
EOF
)"
```

Record the number as `A1_ISSUE`.

- [ ] **Step 2: Branch from the updated main**

```bash
git fetch -q origin && git switch -c <A1_ISSUE>-smt-query-without-ranges origin/main
```

### Task 6 (A1.2): Pass-scoped expectations in `run_test.py`

**Files:**
- Modify: `run_test.py:97` (new regex), `run_test.py:339-440` (`extract_expectations`), `run_test.py:3232-3360` (`check_file`, `evaluate_pass`)

**Interfaces:**
- Produces: fixture syntax `// [default] at line L, column C` and `// [smt-z3] not contains: <text>` (either prefix on either form); `extract_expectations` returns a 9-tuple whose first two items are lists of `(scope, text)` with `scope` in `None | "default" | "smt-z3"`, and whose last item is the list of unknown prefixes.

- [ ] **Step 1: Write the probes that must behave differently**

Create two temporary fixtures (never committed):

```bash
cp test/integer-overflow/nsw-flag-must-not-discharge-itself.c test/integer-overflow/zz-scope-probe.c
printf '\n// [default] not contains: potential signed integer overflow\n' >> test/integer-overflow/zz-scope-probe.c
cp test/integer-overflow/nsw-flag-must-not-discharge-itself.c test/integer-overflow/zz-unknown-scope-probe.c
printf '\n// [smt] not contains: anything\n' >> test/integer-overflow/zz-unknown-scope-probe.c
```

- [ ] **Step 2: Run them to see the current behaviour**

Run: `PYTHONPATH=. python3 "$SCRATCH/smtimprove/check_fixtures.py" test/integer-overflow/zz-scope-probe.c test/integer-overflow/zz-unknown-scope-probe.c` (with `SCRATCH` defined)
Expected: the `[default]` probe line is ignored today (the prefix makes it neither an `at line` block nor a `not contains:`), so both files pass. After Step 3, `zz-scope-probe.c` must fail only in `[pass: default]`, and `zz-unknown-scope-probe.c` must fail with `unknown expectation pass prefix: [smt]`.

- [ ] **Step 3: Implement the prefixes**

After `_RE_STRICT_DETAILS` (line 97):

```python
# `// [smt-z3] not contains: ...` or `// [default] at line ...`: an expectation for one pass.
_RE_PASS_SCOPE = re.compile(r"//\s*\[([A-Za-z0-9_-]+)\]\s+(?=at line|not contains:)")
_EXPECTATION_PASSES = ("default", "smt-z3")
```

In `extract_expectations`, add `unknown_scopes = []` next to `negative_expectations = []`, and insert right before `stripped_line = stripped`:

```python
        scope = None
        scope_match = _RE_PASS_SCOPE.match(stripped)
        if scope_match:
            scope = scope_match.group(1)
            if scope not in _EXPECTATION_PASSES:
                unknown_scopes.append(scope)
            stripped = "// " + stripped[scope_match.end():]
```

Then change `negative_expectations.append(negative)` to `negative_expectations.append((scope, negative))`, change `comment_block = [raw]` to `comment_block = [stripped]`, change `expectations.append(expectation_text)` to `expectations.append((scope, expectation_text))`, and add `unknown_scopes,` as the last item of the returned tuple. Update the docstring:

```python
    """
    Extract expected comment blocks from a .c file.

    Look for comments that start with "// at line" and take all following comment lines.
    Both "// at line" and "// not contains:" accept a pass prefix, "// [default] ..." or
    "// [smt-z3] ...", restricting the expectation to that pass. Expectations are returned as
    (scope, text) pairs, scope None meaning every pass.
    """
```

In `check_file`, add `unknown_scopes,` at the end of the unpacking, and right after it:

```python
    if unknown_scopes:
        known = ", ".join(f"[{p}]" for p in _EXPECTATION_PASSES)
        report_lines.append(
            "  ❌ unknown expectation pass prefix: "
            + ", ".join(f"[{s}]" for s in sorted(set(unknown_scopes)))
            + f" (known: {known})"
        )
        return False, 1, 0, "\n".join(report_lines) + "\n\n"
```

In `evaluate_pass`, add as its first statements:

```python
        applicable = [text for scope, text in expectations if scope in (None, pass_name)]
        applicable_negative = [
            text for scope, text in negative_expectations if scope in (None, pass_name)
        ]
```

and inside `evaluate_pass` only, replace `expectations` with `applicable` and `negative_expectations` with `applicable_negative` in: the `pass_total` computation, the two `enumerate(...)` loops, and the `expected_warning_error` sum.

- [ ] **Step 4: Run the probes**

Run the Step 2 command.
Expected: `zz-scope-probe.c` shows `❌ (default) negative expectation #1 FOUND (unexpected)` and all `(smt-z3)` lines green; `zz-unknown-scope-probe.c` shows `❌ unknown expectation pass prefix: [smt] (known: [default], [smt-z3])`.

- [ ] **Step 5: Remove the probes, run the suite, commit**

```bash
rm test/integer-overflow/zz-scope-probe.c test/integer-overflow/zz-unknown-scope-probe.c
python3 run_test.py --jobs 8 --no-cache
```
Expected: same pass count as `main` plus nothing failing.

```bash
git add run_test.py
git commit -m "test: scope fixture expectations to the default or smt-z3 pass" -m "Refs #A1_ISSUE"
```

### Task 7 (A1.3): Encode lazily

**Files:**
- Modify: `include/analysis/smt/SmtRefinement.hpp:57-94`
- Modify: `src/analysis/StackBufferAnalysis.cpp:104-131`, `src/analysis/OOBReadAnalysis.cpp` (class `OOBReadConstraintEvaluator`), `src/analysis/SizeMinusKWrites.cpp:81-88`, `src/analysis/IntegerOverflowAnalysis.cpp:55-91`, `src/analysis/StackComputation.cpp:347-369`
- Test: `test/unit/analyzer_module_unit_tests.cpp`

**Interfaces:**
- Produces: `template <typename Encode> SmtFeasibility SmtConstraintEvaluator::evaluateQuery(Encode&& encode) const` (protected); `encode` is called only when SMT is on for the rule and must return `ConstraintIR`.

- [ ] **Step 1: Write the failing unit test**

In `test/unit/analyzer_module_unit_tests.cpp`, add `#include "analysis/smt/SmtRefinement.hpp"` with the other project includes, and before `int main`:

```cpp
    /// SMT refinement: an evaluator whose rule has SMT off never builds its query.
    bool testSmtEvaluatorEncodesLazily(TestReport& report)
    {
        struct ProbeEvaluator final : ctrace::stack::analysis::smt::SmtConstraintEvaluator
        {
            using SmtConstraintEvaluator::SmtConstraintEvaluator;

            bool encodes() const
            {
                bool called = false;
                (void)evaluateQuery(
                    [&]
                    {
                        called = true;
                        return ctrace::stack::analysis::smt::ConstraintIR{};
                    });
                return called;
            }
        };

        const ctrace::stack::AnalysisConfig off;
        report.expect(!ProbeEvaluator(off, "stack-buffer").encodes(),
                      "SMT evaluator: SMT off builds no query");

        ctrace::stack::AnalysisConfig otherRule;
        otherRule.smtEnabled = 1;
        otherRule.smtRules = {"recursion"};
        report.expect(!ProbeEvaluator(otherRule, "stack-buffer").encodes(),
                      "SMT evaluator: a rule outside --smt-rules builds no query");

        ctrace::stack::AnalysisConfig on;
        on.smtEnabled = 1;
        report.expect(ProbeEvaluator(on, "stack-buffer").encodes(),
                      "SMT evaluator: SMT on builds the query");
        return report.failures == 0;
    }
```

Register it in `main` after `(void)testProgramPointRanges(repoRoot, report);`:

```cpp
    (void)testSmtEvaluatorEncodesLazily(report);
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build build --target stack_usage_analyzer_unit_tests -j 8`
Expected: compile error, `evaluateQuery` takes a `ConstraintIR`, not a lambda.

- [ ] **Step 3: Make `evaluateQuery` lazy**

In `include/analysis/smt/SmtRefinement.hpp`, replace the `protected:` section (the whole `evaluateQuery` function) with:

```cpp
      protected:
        /// @brief Solves the query built by @p encode.
        ///
        /// When SMT is off for this rule, answers Inconclusive without calling @p encode: the
        /// encoding, and for path-sensitive queries MemorySSA, is only paid for when a solver
        /// will read it.
        template <typename Encode> SmtFeasibility evaluateQuery(Encode&& encode) const
        {
            if (!orchestrator_)
                return SmtFeasibility::Inconclusive;
            return solve(std::forward<Encode>(encode)());
        }

      private:
        SmtFeasibility solve(ConstraintIR ir) const
        {
            SmtQuery query;
            query.ir = std::move(ir);
            query.ruleId = ruleId_;
            query.timeoutMs = timeoutMs_;
            query.budgetNodes = budgetNodes_;

            const SmtDecision decision = orchestrator_->solve(query);
            switch (decision.status)
            {
            case SmtStatus::Sat:
                return SmtFeasibility::Feasible;
            case SmtStatus::Unsat:
                return SmtFeasibility::Infeasible;
            case SmtStatus::Unknown:
            case SmtStatus::Timeout:
            case SmtStatus::Error:
                return SmtFeasibility::Inconclusive;
            }
            return SmtFeasibility::Inconclusive;
        }
```

and remove the now duplicated `private:` label that preceded `std::string ruleId_;`.

- [ ] **Step 4: Wrap every encoder call in a lambda**

Each evaluator method body `return smt::SmtConstraintEvaluator::evaluateQuery(<encode call>);` becomes `return smt::SmtConstraintEvaluator::evaluateQuery([&] { return <encode call>; });`. The complete list:

`src/analysis/StackBufferAnalysis.cpp`, `isNegativeIndexFeasible`:
```cpp
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return smt::encodeSignedComparisonFeasibility(ranges, indexExpr, -1, false,
                                                                      contextInst);
                    });
```
`src/analysis/StackBufferAnalysis.cpp`, `isUpperOverflowFeasible` (after `upperInclusive` is computed):
```cpp
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return smt::encodeSignedComparisonFeasibility(
                            ranges, indexExpr, upperInclusive, true, contextInst);
                    });
```
`src/analysis/OOBReadAnalysis.cpp`, `OOBReadConstraintEvaluator::isNegativeIndexFeasible` and `isUpperOverflowFeasible`: the same two bodies as above.

`src/analysis/SizeMinusKWrites.cpp`, `isSignedLessEqualFeasible`:
```cpp
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return smt::encodeSignedComparisonFeasibility(ranges, lhs, rhsConstant,
                                                                      false, contextInst);
                    });
```
`src/analysis/IntegerOverflowAnalysis.cpp`:
```cpp
            // isSignedOverflowFeasible
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    { return smt::encodeSignedOverflowFeasibility(ranges, binary, contextInst); });
            // isUnsignedOverflowFeasible
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    { return smt::encodeUnsignedOverflowFeasibility(ranges, binary, contextInst); });
            // isSignedGreaterThanFeasible
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return smt::encodeSignedComparisonFeasibility(ranges, lhs, rhsConstant,
                                                                      true, contextInst);
                    });
            // isSignedLessEqualFeasible
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return smt::encodeSignedComparisonFeasibility(ranges, lhs, rhsConstant,
                                                                      false, contextInst);
                    });
```
`src/analysis/StackComputation.cpp`, `RecursionConstraintEvaluator::isSatisfiable`:
```cpp
                const smt::SmtFeasibility feasibility = smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return encoder_.encode(ranges, edgeCondition, takesTrueEdge, edgeBlock,
                                               incomingBlock);
                    });
```

- [ ] **Step 5: Build, format, run**

```bash
/opt/homebrew/opt/llvm@20/bin/clang-format -i include/analysis/smt/SmtRefinement.hpp src/analysis/StackBufferAnalysis.cpp src/analysis/OOBReadAnalysis.cpp src/analysis/SizeMinusKWrites.cpp src/analysis/IntegerOverflowAnalysis.cpp src/analysis/StackComputation.cpp test/unit/analyzer_module_unit_tests.cpp
cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8
./build/stack_usage_analyzer_unit_tests . | grep "SMT evaluator"
```
Expected: three `[PASS] SMT evaluator: ...` lines.

- [ ] **Step 6: Full suite and commit**

Run the unit tests and `python3 run_test.py --jobs 8 --no-cache`. Expected: all green (no result can change: only when the encoding happens changed).

```bash
git add include/analysis/smt/SmtRefinement.hpp src/analysis/StackBufferAnalysis.cpp src/analysis/OOBReadAnalysis.cpp src/analysis/SizeMinusKWrites.cpp src/analysis/IntegerOverflowAnalysis.cpp src/analysis/StackComputation.cpp test/unit/analyzer_module_unit_tests.cpp
git commit -m "perf(smt): build a query only when SMT is on for the rule" -m "Refs #A1_ISSUE"
```

### Task 8 (A1.4): Wrapping semantics

**Files:**
- Modify: `src/analysis/smt/SmtEncoding.cpp:281-308` (`encodeBinaryOperator`)
- Create: `test/integer-overflow/smt-chained-overflow-is-kept.c`

- [ ] **Step 1: Add the guard fixture**

Create `test/integer-overflow/smt-chained-overflow-is-kept.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// Guard: the SMT encoder must not assume `nsw` on the operands of the queried operation.
//
// Only the `sub` is reported: reachesReturn() does not follow arithmetic, so the `add`, which
// overflows for a == INT_MAX, is never checked on its own. Assuming the `add` cannot wrap
// would let the solver discharge the `sub` and hide the only report of this overflow.
//
// This fixture must report in BOTH passes.

int add_then_subtract(int a)
{
    return (a + 1) - 1;
}

// strict-expectation-details: true

// at line 13, column 20
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: sub
// ↳ result is returned without a provable non-overflow bound
```

Run: `PYTHONPATH=. python3 "$SCRATCH/smtimprove/check_fixtures.py" test/integer-overflow/smt-chained-overflow-is-kept.c`
Expected: PASS in both passes (the solver is not asked yet).

- [ ] **Step 2: Show the danger (temporary change, not committed)**

In `src/analysis/IntegerOverflowAnalysis.cpp`, change `if (!queryRanges.empty() &&` (line 837) to `if (`, rebuild `stack_usage_analyzer`, run the Step 1 command.
Expected: FAIL in `[pass: smt-z3]`, `expectation #1 MISSING`: the `nsw` assertion on `a + 1` discharges the `sub`.

- [ ] **Step 3: Encode with wrapping semantics**

In `src/analysis/smt/SmtEncoding.cpp`, `encodeBinaryOperator`, replace everything from `const std::uint32_t bitWidth = inferBitWidth(&binaryOp);` to the final `return result;` with:

```cpp
                // Wrapping semantics, as the -O0 code executes. nsw/nuw promise that an operand
                // does not wrap, which is exactly what may be false where a warning is due, so
                // they never become assertions.
                return builder_.makeBinary(opKind, *lhs, *rhs, inferBitWidth(&binaryOp));
```

Rebuild, run the Step 1 command.
Expected: PASS in both passes.

- [ ] **Step 4: Revert the temporary change**

```bash
git checkout -- src/analysis/IntegerOverflowAnalysis.cpp
```

- [ ] **Step 5: Full suite and commit**

Format `src/analysis/smt/SmtEncoding.cpp`, rebuild, run the unit tests and `python3 run_test.py --jobs 8 --no-cache`. Expected: all green.

```bash
git add src/analysis/smt/SmtEncoding.cpp test/integer-overflow/smt-chained-overflow-is-kept.c
git commit -m "fix(smt): encode arithmetic with wrapping semantics" -m "Refs #A1_ISSUE"
```

### Task 9 (A1.5): Ask the solver without a known range

**Files:**
- Modify: `src/analysis/IntegerOverflowAnalysis.cpp:754`, `:766`, `:782`, `:837`
- Modify: `src/analysis/SizeMinusKWrites.cpp:493-501`
- Modify: `src/analysis/OOBReadAnalysis.cpp:347-348`
- Modify: `src/analysis/StackBufferAnalysis.cpp:691-692`, `:708-709`
- Create: `test/integer-overflow/smt-byte-operand-cannot-overflow.c`, `test/integer-overflow/smt-size-times-one-cannot-overflow.c`

- [ ] **Step 1: Write the failing fixtures**

Create `test/integer-overflow/smt-byte-operand-cannot-overflow.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// SMT refinement asks the solver even when no range is known.
//
// `1 + c->n` widens an unsigned char before adding, so the sum is at most 256 and cannot
// overflow an int. The range analysis has no bound for a byte loaded from memory, so the
// default pass reports the addition; the solver proves it safe from the bit widths alone.

struct counter
{
    unsigned char n;
};

int byte_plus_one(const struct counter* c)
{
    return 1 + c->n;
}

// [default] at line 16, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
```

Create `test/integer-overflow/smt-size-times-one-cannot-overflow.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// SMT refinement asks the solver even when no range is known.
//
// `l * sizeof(char)` multiplies by one and cannot wrap. With no range for `l`, the default
// pass reports the size computation; the solver proves it safe without any range.

#include <string.h>

void copy_chars(char* dst, const char* src, unsigned long l)
{
    memcpy(dst, src, l * sizeof(char));
}

// [default] at line 12, column 5
// [ !!Warn ] potential integer overflow in size computation before 'memcpy'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// [smt-z3] not contains: potential integer overflow in size computation
```

- [ ] **Step 2: Run them to see them fail**

Run: `PYTHONPATH=. python3 "$SCRATCH/smtimprove/check_fixtures.py" test/integer-overflow/smt-byte-operand-cannot-overflow.c test/integer-overflow/smt-size-times-one-cannot-overflow.c`
Expected: `[pass: default]` green; `[pass: smt-z3]` fails on the negative expectation and on the strict count (1 found, 0 expected).

- [ ] **Step 3: Remove the eight preconditions**

`src/analysis/IntegerOverflowAnalysis.cpp`: delete the two lines `if (queryRanges.empty())` / `return false;` at 754, 766 and 782, and at 837 change

```cpp
                            if (!queryRanges.empty() &&
                                evaluator.isSignedOverflowFeasible(queryRanges, *binary, &inst) ==
                                    SmtFeasibility::Infeasible)
```
to
```cpp
                            if (evaluator.isSignedOverflowFeasible(queryRanges, *binary, &inst) ==
                                SmtFeasibility::Infeasible)
```

`src/analysis/SizeMinusKWrites.cpp`: replace the `if (!queryRanges.empty()) { ... }` block with

```cpp
                    if (evaluator.isSignedLessEqualFeasible(queryRanges, *sizeBase, k, at) ==
                        SmtFeasibility::Infeasible)
                        issue.sizeAboveK = true;
```

`src/analysis/OOBReadAnalysis.cpp`: delete `if (queryRanges.empty())` / `return false;` in `isHeapIndexViolationInfeasibleBySmt`.

`src/analysis/StackBufferAnalysis.cpp`: delete `if (!localRange.hasLower && !localRange.hasUpper)` / `return false;` in `isUpperViolationInfeasibleBySmt` and in `isLowerViolationInfeasibleBySmt`.

- [ ] **Step 4: Run the fixtures**

Format the four files, rebuild `stack_usage_analyzer`, run the Step 2 command plus `test/integer-overflow/smt-chained-overflow-is-kept.c` and `test/integer-overflow/nsw-flag-must-not-discharge-itself.c`.
Expected: all PASS in both passes.

- [ ] **Step 5: Full suite**

Run the unit tests and `python3 run_test.py --jobs 8 --no-cache`.
Expected: all green. If an existing fixture fails in `[pass: smt-z3]` because a diagnostic disappeared, do not touch it: list the file, the diagnostic and why it is a false positive, and ask the user before changing its expectation to `[default]`.

- [ ] **Step 6: Commit**

```bash
git add src/analysis/IntegerOverflowAnalysis.cpp src/analysis/SizeMinusKWrites.cpp src/analysis/OOBReadAnalysis.cpp src/analysis/StackBufferAnalysis.cpp test/integer-overflow/smt-byte-operand-cannot-overflow.c test/integer-overflow/smt-size-times-one-cannot-overflow.c
git commit -m "feat(smt): ask the solver even when no range is known" -m "Refs #A1_ISSUE"
```

### Task 10 (A1.6): Validate, measure, report, open the PR

- [ ] **Step 1: Full validation** — build, unit tests, `python3 run_test.py --jobs 8 --no-cache`, the clang-format check of Task A0.4 Step 1. Expected: all green.
- [ ] **Step 2: Measure** — run Procedure M. Expected: SMT off identical diagnostics; SMT z3 removes the 8 Lua false positives of spec §4.5 (`lauxlib.c:594`, `lauxlib.c:613`, `lstring.c:238`, `lstring.c:257`, `lgc.c:637`, `lgc.c:665`, `lgc.c:679`, `lparser.c:240`); every removed diagnostic labelled; no true positive removed.
- [ ] **Step 3: Push and open the PR** — push the branch, then `gh pr create --base main --title "feat(smt): query without a known range, with wrapping semantics"` with a body in the format of Task A0.4 (`Closes #A1_ISSUE`, design and plan links, `What`, `Regression report` filled from Procedure M).
- [ ] **Step 4: Stop** — report to the user and wait for the merge before Part A2.

---

## Part A2 — Path conditions over a MemorySSA memory model

### Task 11 (A2.1): Issue and branch

- [ ] **Step 1: Create the issue**

```bash
gh issue create --title "SMT: path conditions and a MemorySSA memory model" --body "$(cat <<'EOF'
## Problem
At -O0 every read of a local is a distinct `load`, encoded as a free symbol, and queries carry no branch condition. The `i` of a guard and the `i` of the access it protects are unrelated unknowns, so guards never reach the solver: the CERT INT32-C check before `a + b`, `if (n > SIZE_MAX / 8) return 0;` before `malloc(n * 8)`, and `i <= 20, i < n, n <= 16` before `buf[i]` stay reported. Encoded by hand with their path condition, Z3 proves all three safe.

## Change
- `FunctionFacts` gains BasicAA and MemorySSA, built on demand, behind one query: `clobberingAccess(load)` (steps 1-2 of #80).
- Loads are encoded through their clobber: the stored value, or one symbol per (pointer, clobber).
- Each query of integer-overflow, size-minus-k, stack-buffer and oob-read carries the reachability condition of its instruction over the CFG without back edges, from the farthest dominator that fits the node budget.

Design: docs/superpowers/specs/2026-09-24-smt-solver-precision-design.md (§5).
EOF
)"
```

Record the number as `A2_ISSUE`.

- [ ] **Step 2: Branch from the updated main**

```bash
git fetch -q origin && git switch -c <A2_ISSUE>-smt-path-conditions origin/main
```

### Task 12 (A2.2): `FunctionFacts::clobberingAccess`

**Files:**
- Modify: `include/analysis/FunctionFacts.hpp`, `src/analysis/FunctionFacts.cpp`
- Create: `test/unit/smt_path_input.c`
- Test: `test/unit/analyzer_module_unit_tests.cpp`

**Interfaces:**
- Produces: `[[nodiscard]] const llvm::MemoryAccess* ctrace::stack::analysis::FunctionFacts::clobberingAccess(const llvm::LoadInst& load) const`.

- [ ] **Step 1: Create the unit-test input**

Create `test/unit/smt_path_input.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Input for the SMT memory-model and path-condition unit tests
// (test/unit/analyzer_module_unit_tests.cpp).

void opaque_write(int* p);

int reads_param_twice(int x)
{
    int a = x;
    int b = x;
    return a - b;
}

int reads_across_calls(void)
{
    int x = 0;
    opaque_write(&x);
    int before = x;
    opaque_write(&x);
    int after = x;
    return after - before;
}
```

- [ ] **Step 2: Write the failing unit test**

In `test/unit/analyzer_module_unit_tests.cpp`, add `#include <llvm/Analysis/MemorySSA.h>` and `#include <llvm/IR/InstIterator.h>` with the LLVM includes, then before `int main`:

```cpp
    /// Loads of @p fn whose pointer operand is named @p slot, in instruction order.
    std::vector<const llvm::LoadInst*> loadsFrom(const llvm::Function* fn, llvm::StringRef slot)
    {
        std::vector<const llvm::LoadInst*> out;
        if (!fn)
            return out;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
            if (load && load->getPointerOperand()->getName() == slot)
                out.push_back(load);
        }
        return out;
    }

    /// FunctionFacts::clobberingAccess: MemorySSA clobbers of -O0 slot reads.
    bool testFunctionFactsClobberingAccess(const std::filesystem::path& repoRoot,
                                           TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "clobberingAccess setup: failed to load module: " + loadError);
            return false;
        }

        {
            llvm::Function* fn = loaded.module->getFunction("reads_param_twice");
            const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "x.addr");
            report.expect(loads.size() == 2, "clobberingAccess: reads_param_twice reads x twice");
            if (loads.size() == 2)
            {
                const FunctionFacts facts(*fn);
                const llvm::MemoryAccess* first = facts.clobberingAccess(*loads[0]);
                report.expect(first != nullptr && first == facts.clobberingAccess(*loads[1]),
                              "clobberingAccess: two reads with no write between share it");
                const auto* def = llvm::dyn_cast_or_null<llvm::MemoryDef>(first);
                const auto* store =
                    def ? llvm::dyn_cast_or_null<llvm::StoreInst>(def->getMemoryInst()) : nullptr;
                report.expect(store && llvm::isa<llvm::Argument>(store->getValueOperand()),
                              "clobberingAccess: it is the store of the parameter");
            }
        }

        {
            llvm::Function* fn = loaded.module->getFunction("reads_across_calls");
            const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "x");
            report.expect(loads.size() == 2, "clobberingAccess: reads_across_calls reads x twice");
            if (loads.size() == 2)
            {
                const FunctionFacts facts(*fn);
                const auto* first =
                    llvm::dyn_cast_or_null<llvm::MemoryDef>(facts.clobberingAccess(*loads[0]));
                const auto* second =
                    llvm::dyn_cast_or_null<llvm::MemoryDef>(facts.clobberingAccess(*loads[1]));
                report.expect(first && second && first != second &&
                                  llvm::isa_and_nonnull<llvm::CallBase>(first->getMemoryInst()) &&
                                  llvm::isa_and_nonnull<llvm::CallBase>(second->getMemoryInst()),
                              "clobberingAccess: a call that may write the slot separates reads");
            }
        }
        return report.failures == 0;
    }
```

Register it in `main` after `testSmtEvaluatorEncodesLazily`:

```cpp
    (void)testFunctionFactsClobberingAccess(repoRoot, report);
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake --build build --target stack_usage_analyzer_unit_tests -j 8`
Expected: compile error, `FunctionFacts` has no member `clobberingAccess`.

- [ ] **Step 4: Implement**

`include/analysis/FunctionFacts.hpp`: add `class LoadInst;` and `class MemoryAccess;` to the `namespace llvm` forward declarations, and after `objectSizeBytes`:

```cpp
        /// @brief Access that may last have written the memory read by @p load (MemorySSA
        /// clobber).
        ///
        /// BasicAA and MemorySSA are built on the first call, so functions that never ask pay
        /// nothing. Loads of one pointer with one clobber read the same value; a clobber that
        /// stores to that pointer is the value they read.
        [[nodiscard]] const llvm::MemoryAccess* clobberingAccess(const llvm::LoadInst& load) const;
```

`src/analysis/FunctionFacts.cpp`: add `#include <llvm/Analysis/AliasAnalysis.h>`, `#include <llvm/Analysis/BasicAliasAnalysis.h>`, `#include <llvm/Analysis/MemorySSA.h>`; in the anonymous namespace add:

```cpp
        /// BasicAA and the MemorySSA built on it. MemorySSA keeps a pointer to the AA, so both
        /// live together.
        struct MemoryModel final
        {
            MemoryModel(llvm::Function& function, const llvm::DataLayout& dataLayout,
                        const llvm::TargetLibraryInfo& libraryInfo,
                        llvm::AssumptionCache& assumptions, llvm::DominatorTree& dominators)
                : basicAA(dataLayout, function, libraryInfo, assumptions, &dominators),
                  aliasAnalysis(libraryInfo)
            {
                aliasAnalysis.addAAResult(basicAA);
                memorySSA =
                    std::make_unique<llvm::MemorySSA>(function, &aliasAnalysis, &dominators);
            }

            llvm::BasicAAResult basicAA;
            llvm::AAResults aliasAnalysis;
            std::unique_ptr<llvm::MemorySSA> memorySSA;
        };
```

add as the last member of `FunctionFacts::Impl` (so it is destroyed before the analyses it references):

```cpp
        std::unique_ptr<MemoryModel> memoryModel;
```

and after `objectSizeBytes`:

```cpp
    const llvm::MemoryAccess* FunctionFacts::clobberingAccess(const llvm::LoadInst& load) const
    {
        Impl& impl = *impl_;
        if (!impl.memoryModel)
        {
            impl.memoryModel = std::make_unique<MemoryModel>(
                impl.function, impl.dataLayout, impl.targetLibraryInfo, impl.assumptionCache,
                impl.dominatorTree);
        }
        return impl.memoryModel->memorySSA->getWalker()->getClobberingMemoryAccess(&load);
    }
```

- [ ] **Step 5: Build, format, run**

Format the three C++ files, build all targets, run `./build/stack_usage_analyzer_unit_tests . | grep clobberingAccess`.
Expected: five `[PASS] clobberingAccess: ...` lines.

- [ ] **Step 6: Commit**

```bash
git add include/analysis/FunctionFacts.hpp src/analysis/FunctionFacts.cpp test/unit/smt_path_input.c test/unit/analyzer_module_unit_tests.cpp
git commit -m "feat(analysis): expose MemorySSA clobbers through FunctionFacts" -m "Refs #A2_ISSUE"
```

### Task 13 (A2.3): Memory-aware load encoding

**Files:**
- Modify: `include/analysis/smt/SmtEncoding.hpp`
- Modify: `src/analysis/smt/SmtEncoding.cpp` (builder, `LlvmExprEncoder`, `encodeRangeAssertions`, new `encodeQuery`, the three public query functions)
- Modify: the evaluator methods of Task A1.3 Step 4 (four rule files)
- Modify: `test/unit/smt_path_input.c`, `test/unit/analyzer_module_unit_tests.cpp`

**Interfaces:**
- Consumes: `FunctionFacts::clobberingAccess` (Task A2.2).
- Produces:

```cpp
namespace ctrace::stack::analysis::smt
{
    struct QueryPoint
    {
        const llvm::Instruction* inst = nullptr;
        const FunctionFacts* facts = nullptr;
        std::uint64_t budgetNodes = 0;
    };

    ConstraintIR encodeSignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                                 const llvm::BinaryOperator& binaryOperation,
                                                 const QueryPoint& point = {});
    ConstraintIR encodeUnsignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                                   const llvm::BinaryOperator& binaryOperation,
                                                   const QueryPoint& point = {});
    ConstraintIR encodeSignedComparisonFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                                   const llvm::Value& lhs, std::int64_t rhsConstant,
                                                   bool greaterThan, const QueryPoint& point = {});
}
```

- [ ] **Step 1: Extend the unit-test input**

Append to `test/unit/smt_path_input.c`:

```c
int shared_counter;
volatile int sensor;
void touch_counter(void);

int global_read_twice(void)
{
    return shared_counter - shared_counter;
}

int global_read_across_call(void)
{
    int before = shared_counter;
    touch_counter();
    return shared_counter - before;
}

int forwarded_local(int x)
{
    int y = x;
    return y - x;
}

int punned_store(void)
{
    int x = 0;
    *(char*)&x = 5;
    return x - 1;
}

int volatile_read_twice(void)
{
    return sensor - sensor;
}
```

- [ ] **Step 2: Write the failing unit test**

In `test/unit/analyzer_module_unit_tests.cpp`, add `#include "analysis/smt/SmtEncoding.hpp"` and `#include <algorithm>`, `#include <optional>`, then before `int main`:

```cpp
    using ctrace::stack::analysis::smt::ConstraintIR;
    using ctrace::stack::analysis::smt::ExprId;
    using ctrace::stack::analysis::smt::ExprKind;
    using ctrace::stack::analysis::smt::ExprNode;
    using ctrace::stack::analysis::smt::QueryPoint;

    /// First binary operator of @p fn with @p opcode, or nullptr.
    const llvm::BinaryOperator* firstBinary(const llvm::Function* fn, unsigned opcode)
    {
        if (!fn)
            return nullptr;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
            if (binary && binary->getOpcode() == opcode)
                return binary;
        }
        return nullptr;
    }

    /// Last binary operator of @p fn with @p opcode, or nullptr.
    const llvm::BinaryOperator* lastBinary(const llvm::Function* fn, unsigned opcode)
    {
        const llvm::BinaryOperator* last = nullptr;
        if (!fn)
            return last;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
            if (binary && binary->getOpcode() == opcode)
                last = binary;
        }
        return last;
    }

    /// First node of @p ir with @p kind, or nullptr.
    const ExprNode* firstNode(const ConstraintIR& ir, ExprKind kind)
    {
        const auto it = std::find_if(ir.nodes.begin(), ir.nodes.end(),
                                     [&](const ExprNode& node) { return node.kind == kind; });
        return it == ir.nodes.end() ? nullptr : &*it;
    }

    /// Left operand of the first @p kind node whose right operand is the constant @p value.
    std::optional<ExprId> lhsAgainstConstant(const ConstraintIR& ir, ExprKind kind,
                                             std::int64_t value)
    {
        for (const ExprNode& node : ir.nodes)
        {
            if (node.kind != kind)
                continue;
            const ExprNode& rhs = ir.nodes.at(node.rhs);
            if (rhs.kind == ExprKind::Constant && rhs.constant == value)
                return node.lhs;
        }
        return std::nullopt;
    }

    /// Whether @p ir negates a @p kind comparison against the constant @p value.
    bool hasNegatedComparison(const ConstraintIR& ir, ExprKind kind, std::int64_t value)
    {
        return std::any_of(ir.nodes.begin(), ir.nodes.end(),
                           [&](const ExprNode& node)
                           {
                               if (node.kind != ExprKind::Not)
                                   return false;
                               const ExprNode& operand = ir.nodes.at(node.lhs);
                               if (operand.kind != kind)
                                   return false;
                               const ExprNode& rhs = ir.nodes.at(operand.rhs);
                               return rhs.kind == ExprKind::Constant && rhs.constant == value;
                           });
    }

    /// SMT encoder: loads are encoded through their MemorySSA clobber.
    bool testSmtEncoderMemoryModel(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "SMT memory model setup: failed to load module: " + loadError);
            return false;
        }

        // The query on `lhs - rhs` rebuilds the `sub` from its encoded operands, so the first
        // Sub node of the IR says how each operand was encoded.
        const auto subOperands =
            [&](const char* name, bool withFacts) -> std::optional<std::pair<ExprId, ExprId>>
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            if (!sub)
                return std::nullopt;
            const FunctionFacts facts(*fn);
            const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                {}, *sub, QueryPoint{.inst = sub, .facts = withFacts ? &facts : nullptr});
            const ExprNode* node = firstNode(ir, ExprKind::Sub);
            if (!node)
                return std::nullopt;
            return std::make_pair(node->lhs, node->rhs);
        };

        {
            const auto with = subOperands("global_read_twice", true);
            const auto without = subOperands("global_read_twice", false);
            report.expect(with && with->first == with->second,
                          "SMT memory model: two reads with no write between are one symbol");
            report.expect(without && without->first != without->second,
                          "SMT memory model: without facts every load stays its own symbol");
        }
        {
            const auto with = subOperands("global_read_across_call", true);
            report.expect(with && with->first != with->second,
                          "SMT memory model: a call that may write the global separates reads");
        }
        {
            llvm::Function* fn = loaded.module->getFunction("forwarded_local");
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            const auto with = subOperands("forwarded_local", true);
            report.expect(with && with->first == with->second,
                          "SMT memory model: a load after a store reads the stored value");
            if (fn && sub)
            {
                const FunctionFacts facts(*fn);
                const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                    {}, *sub, QueryPoint{.inst = sub, .facts = &facts});
                const ExprNode* node = firstNode(ir, ExprKind::Sub);
                const ExprNode* operand = node ? &ir.nodes.at(node->lhs) : nullptr;
                const bool isParameter =
                    operand && operand->kind == ExprKind::Symbol &&
                    std::any_of(ir.symbols.begin(), ir.symbols.end(),
                                [&](const auto& symbol)
                                { return symbol.id == operand->symbol && symbol.debugName == "x"; });
                report.expect(isParameter,
                              "SMT memory model: forwarding reaches the parameter itself");

                // Ranges must bound the very expression the violation uses.
                const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "y");
                if (!loads.empty())
                {
                    std::map<const llvm::Value*, IntRange> ranges;
                    ranges[loads[0]] =
                        IntRange{.lower = 0, .upper = 3, .hasLower = true, .hasUpper = true};
                    const ConstraintIR ranged = smt::encodeSignedComparisonFeasibility(
                        ranges, *loads[0], 15, true, QueryPoint{.inst = sub, .facts = &facts});
                    const std::optional<ExprId> bounded =
                        lhsAgainstConstant(ranged, ExprKind::Sle, 3);
                    const std::optional<ExprId> violated =
                        lhsAgainstConstant(ranged, ExprKind::Sgt, 15);
                    report.expect(bounded && violated && *bounded == *violated,
                                  "SMT memory model: a range on a load bounds its encoding");
                }
            }
        }
        {
            // Review focus 1: an i8 store does not define the i32 read that follows it.
            llvm::Function* fn = loaded.module->getFunction("punned_store");
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            if (fn && sub)
            {
                const FunctionFacts facts(*fn);
                const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                    {}, *sub, QueryPoint{.inst = sub, .facts = &facts});
                const ExprNode* node = firstNode(ir, ExprKind::Sub);
                report.expect(node && ir.nodes.at(node->lhs).kind == ExprKind::Symbol &&
                                  !lhsAgainstConstant(ir, ExprKind::Eq, 5),
                              "SMT memory model: a narrower store is not forwarded");
            }
            else
            {
                report.expect(false, "SMT memory model: punned_store has a sub");
            }
        }
        {
            // Review focus 2: volatile reads never share a symbol.
            const auto with = subOperands("volatile_read_twice", true);
            report.expect(with && with->first != with->second,
                          "SMT memory model: volatile reads stay distinct");
        }
        return report.failures == 0;
    }
```

Register it in `main` after `testFunctionFactsClobberingAccess`:

```cpp
    (void)testSmtEncoderMemoryModel(repoRoot, report);
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake --build build --target stack_usage_analyzer_unit_tests -j 8`
Expected: compile error, `QueryPoint` is not declared.

- [ ] **Step 4: Add `QueryPoint` to the header**

`include/analysis/smt/SmtEncoding.hpp`: before `namespace ctrace::stack::analysis::smt`, add

```cpp
namespace ctrace::stack::analysis
{
    class FunctionFacts;
} // namespace ctrace::stack::analysis
```

inside the smt namespace, before `LlvmConstraintEncoder`:

```cpp
    /// @brief Where a query is asked, and what the encoder may use to encode it.
    struct QueryPoint
    {
        /// Instruction the query is about; nullptr encodes the bare query.
        const llvm::Instruction* inst = nullptr;
        /// Facts of the function holding @ref inst. When set, loads are related through
        /// MemorySSA and the query carries the reachability condition of @ref inst.
        const FunctionFacts* facts = nullptr;
        /// Largest query, in ConstraintIR nodes, the path condition may grow it to; 0 = none.
        std::uint64_t budgetNodes = 0;
    };
```

and replace the three declarations' last parameter `const llvm::Instruction* contextInst = nullptr` with `const QueryPoint& point = {}` (see Interfaces).

- [ ] **Step 5: Memory symbols in the builder**

`src/analysis/smt/SmtEncoding.cpp`: add `#include "analysis/FunctionFacts.hpp"`, `#include <llvm/Analysis/MemorySSA.h>`, `#include <map>`, `#include <tuple>`. In `ConstraintIrBuilder`, add after `makeSymbol`:

```cpp
            /// One symbol per (pointer, clobbering access, width): the loads it stands for read
            /// the same memory, hence the same value.
            ExprId makeMemorySymbol(const llvm::Value* pointer, const void* access,
                                    std::uint32_t bitWidth)
            {
                const MemoryKey key{pointer, access, bitWidth};
                if (const auto it = memorySymbols_.find(key); it != memorySymbols_.end())
                    return it->second;

                const SymbolId id = nextSymbolId_++;
                const ExprId expr = appendNode(ExprNode{.kind = ExprKind::Symbol,
                                                        .symbol = id,
                                                        .constant = 0,
                                                        .bitWidth = normalizeBitWidth(bitWidth),
                                                        .lhs = 0,
                                                        .rhs = 0,
                                                        .extra = 0});
                memorySymbols_.emplace(key, expr);
                ir_.symbols.push_back(SymbolInfo{.id = id,
                                                 .debugName = buildSymbolName(pointer, id) + "@mem",
                                                 .sourceToken = toSourceToken(pointer)});
                return expr;
            }
```

and in its private section:

```cpp
            using MemoryKey = std::tuple<const llvm::Value*, const void*, std::uint32_t>;
            std::map<MemoryKey, ExprId> memorySymbols_;
```

- [ ] **Step 6: Encode loads through their clobber**

In `LlvmExprEncoder`, change the constructor:

```cpp
            LlvmExprEncoder(ConstraintIrBuilder& builder, const llvm::BasicBlock* incomingBlock,
                            const FunctionFacts* facts = nullptr)
                : builder_(builder), incomingBlock_(incomingBlock), facts_(facts)
            {
            }
```

and declare the member right after `incomingBlock_`, so members initialize in declaration order:

```cpp
            const llvm::BasicBlock* incomingBlock_ = nullptr;
            const FunctionFacts* facts_ = nullptr;
```

In `encodeValueImpl`, before the final `return builder_.makeSymbol(&value, inferBitWidth(&value));`:

```cpp
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&value);
                    load && facts_ && load->isSimple() && load->getType()->isIntegerTy())
                {
                    return encodeLoad(*load);
                }
```

and add as a private member function:

```cpp
            /// A load reads what its MemorySSA clobber left in memory: the stored value when the
            /// clobber stores the same type to the same pointer, otherwise one value shared by
            /// every load of that pointer with that clobber.
            std::optional<ExprId> encodeLoad(const llvm::LoadInst& load)
            {
                const llvm::Value* pointer = load.getPointerOperand()->stripPointerCasts();
                const llvm::MemoryAccess* clobber = facts_->clobberingAccess(load);
                if (!clobber)
                    return builder_.makeSymbol(&load, inferBitWidth(&load));

                if (const auto* def = llvm::dyn_cast<llvm::MemoryDef>(clobber))
                {
                    const auto* store =
                        llvm::dyn_cast_or_null<llvm::StoreInst>(def->getMemoryInst());
                    if (store && store->isSimple() &&
                        store->getPointerOperand()->stripPointerCasts() == pointer &&
                        store->getValueOperand()->getType() == load.getType())
                    {
                        return encodeValue(store->getValueOperand());
                    }
                }
                return builder_.makeMemorySymbol(pointer, clobber, inferBitWidth(&load));
            }
```

- [ ] **Step 7: Bound the encoded expression, not a fresh symbol**

Replace the body of the loop in `encodeRangeAssertions` with:

```cpp
                if (!shouldEncodeRangeConstraint(value, range))
                    continue;

                // The bounds constrain the expression the rest of the query uses for `value`: a
                // load, for one, may be encoded as the value it reads rather than as a symbol.
                const std::optional<ExprId> expr = exprEncoder.encodeValue(value);
                if (!expr)
                    continue;

                const ExprKind kind = builder.node(*expr).kind;
                const SymbolId symbol = builder.node(*expr).symbol;
                const std::uint32_t bitWidth = builder.node(*expr).bitWidth;
                if (kind == ExprKind::Symbol)
                {
                    ir.intervals.push_back(
                        IntervalConstraint{.symbol = symbol,
                                           .lower = static_cast<std::int64_t>(range.lower),
                                           .upper = static_cast<std::int64_t>(range.upper),
                                           .hasLower = range.hasLower,
                                           .hasUpper = range.hasUpper});
                }
                if (range.hasLower)
                {
                    const ExprId lower =
                        builder.makeConstant(static_cast<std::int64_t>(range.lower), bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sge, *expr, lower, 1));
                }
                if (range.hasUpper)
                {
                    const ExprId upper =
                        builder.makeConstant(static_cast<std::int64_t>(range.upper), bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sle, *expr, upper, 1));
                }
```

- [ ] **Step 8: Route the public queries through `encodeQuery`**

Add after `encodeWithCustomAssertions`:

```cpp
        /// Builds the query at @p point: the rule's ranges, then what @p postEncode asserts.
        static ConstraintIR encodeQuery(const std::map<const llvm::Value*, IntRange>& ranges,
                                        const QueryPoint& point,
                                        const QueryPostEncoder& postEncode)
        {
            ConstraintIR ir;
            ir.intervals.reserve(ranges.size());
            ConstraintIrBuilder builder(ir);
            LlvmExprEncoder exprEncoder(builder, nullptr, point.facts);
            encodeRangeAssertions(ranges, ir, builder, exprEncoder);
            postEncode(builder, exprEncoder);
            return ir;
        }
```

In `encodeSignedOverflowFeasibility`, `encodeUnsignedOverflowFeasibility` and `encodeSignedComparisonFeasibility`: replace the last parameter `const llvm::Instruction* contextInst` with `const QueryPoint& point`, replace `return encodeWithCustomAssertions(ranges, nullptr, true, nullptr, nullptr, [&](...) {...});` with `return encodeQuery(ranges, point, [&](...) {...});` keeping the lambda body, and inside it replace `encodeAssumesBeforeInstruction(contextInst, ...)` with `encodeAssumesBeforeInstruction(point.inst, ...)`.

- [ ] **Step 9: Keep the rules unchanged for now**

In every evaluator lambda of Task A1.3 Step 4 except the recursion one, replace the last argument `contextInst` with `smt::QueryPoint{.inst = contextInst}` (no facts yet: results must not change in this task).

- [ ] **Step 10: Build, format, run**

Format the modified files, build all targets, run `./build/stack_usage_analyzer_unit_tests . | grep "SMT memory model"`.
Expected: nine `[PASS] SMT memory model: ...` lines.

- [ ] **Step 11: Full suite and commit**

Run the unit tests and `python3 run_test.py --jobs 8 --no-cache`. Expected: all green.

```bash
git add include/analysis/smt/SmtEncoding.hpp src/analysis/smt/SmtEncoding.cpp src/analysis/StackBufferAnalysis.cpp src/analysis/OOBReadAnalysis.cpp src/analysis/SizeMinusKWrites.cpp src/analysis/IntegerOverflowAnalysis.cpp test/unit/smt_path_input.c test/unit/analyzer_module_unit_tests.cpp
git commit -m "feat(smt): encode loads through their MemorySSA clobber" -m "Refs #A2_ISSUE"
```

### Task 14 (A2.4): Reachability condition

**Files:**
- Modify: `src/analysis/smt/SmtEncoding.cpp` (new helpers; `encodeQuery` replaced)
- Modify: `test/unit/smt_path_input.c`, `test/unit/analyzer_module_unit_tests.cpp`

**Interfaces:**
- Consumes: `QueryPoint`, `encodeQuery` (Task A2.3), `FunctionFacts::dominatorTree()`.
- Produces: every query built with `QueryPoint{.inst, .facts}` carries `reach(block of inst)` unless the function is irreducible, `inst` is in the entry block, or even the immediate dominator exceeds `budgetNodes`.

- [ ] **Step 1: Extend the unit-test input**

Append to `test/unit/smt_path_input.c`:

```c
int guarded_increment(int i)
{
    if (i > 5)
        return 0;
    return i + 1;
}

int either_positive(int a, int b)
{
    if (a > 0 || b > 0)
        return a + b;
    return 0;
}

int irreducible_loop(int n, int k)
{
    if (n > 0)
        goto inside;
top:
    k = k + 1;
inside:
    if (k < 10)
        goto top;
    return k + n;
}

int switch_case(int x, int y)
{
    switch (x)
    {
    case 1:
    case 2:
        return y + 1;
    default:
        return 0;
    }
}

int entry_block_add(int a)
{
    return a + 1;
}
```

- [ ] **Step 2: Write the failing unit test**

Before `int main`:

```cpp
    /// SMT encoder: queries carry the reachability condition of their instruction.
    bool testSmtEncoderPathCondition(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "SMT path condition setup: failed to load module: " + loadError);
            return false;
        }

        // Overflow query on @p add, with or without the function's facts.
        const auto query = [&](const char* name, bool last, bool withFacts,
                               std::uint64_t budget) -> std::optional<ConstraintIR>
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            const llvm::BinaryOperator* add = last ? lastBinary(fn, llvm::Instruction::Add)
                                                   : firstBinary(fn, llvm::Instruction::Add);
            if (!add)
                return std::nullopt;
            const FunctionFacts facts(*fn);
            return smt::encodeSignedOverflowFeasibility(
                {}, *add,
                QueryPoint{.inst = add,
                           .facts = withFacts ? &facts : nullptr,
                           .budgetNodes = budget});
        };

        {
            const auto with = query("guarded_increment", false, true, 0);
            const auto without = query("guarded_increment", false, false, 0);
            report.expect(with && hasNegatedComparison(*with, ExprKind::Sgt, 5),
                          "SMT path condition: the false edge of `i > 5` guards the add");
            report.expect(without && !hasNegatedComparison(*without, ExprKind::Sgt, 5),
                          "SMT path condition: without facts the query has no path condition");
            const auto tight = query("guarded_increment", false, true, 1);
            report.expect(tight && !hasNegatedComparison(*tight, ExprKind::Sgt, 5),
                          "SMT path condition: a region over budget falls back to no condition");
        }
        {
            const auto with = query("either_positive", false, true, 0);
            report.expect(with && firstNode(*with, ExprKind::Or) != nullptr,
                          "SMT path condition: a block with two predecessors gives a disjunction");
        }
        {
            const auto with = query("irreducible_loop", true, true, 0);
            report.expect(with && !lhsAgainstConstant(*with, ExprKind::Slt, 10) &&
                              !lhsAgainstConstant(*with, ExprKind::Sgt, 0) &&
                              firstNode(*with, ExprKind::Not) == nullptr,
                          "SMT path condition: an irreducible function gets no path condition");
        }
        {
            // Review focus 3: two cases reach the block; the default edge does not.
            const auto with = query("switch_case", false, true, 0);
            report.expect(with && lhsAgainstConstant(*with, ExprKind::Eq, 1) &&
                              lhsAgainstConstant(*with, ExprKind::Eq, 2) &&
                              firstNode(*with, ExprKind::Or) != nullptr,
                          "SMT path condition: switch cases reaching a block are a disjunction");
        }
        {
            // Review focus 4: an instruction of the entry block has no dominator.
            const auto with = query("entry_block_add", false, true, 0);
            report.expect(with && !with->assertions.empty() &&
                              firstNode(*with, ExprKind::Not) == nullptr &&
                              firstNode(*with, ExprKind::Or) == nullptr,
                          "SMT path condition: an entry-block query has no path condition");
        }
        return report.failures == 0;
    }
```

Register it in `main` after `testSmtEncoderMemoryModel`:

```cpp
    (void)testSmtEncoderPathCondition(repoRoot, report);
```

- [ ] **Step 3: Run it to see it fail**

Build the unit tests and run `./build/stack_usage_analyzer_unit_tests . | grep "SMT path condition"`.
Expected: FAIL on `the false edge of i > 5 guards the add`, `a block with two predecessors gives a disjunction` and `switch cases reaching a block are a disjunction`.

- [ ] **Step 4: Implement the reachability condition**

`src/analysis/smt/SmtEncoding.cpp`: add `#include <llvm/ADT/PostOrderIterator.h>`, `#include <llvm/Analysis/CFG.h>`, `#include <llvm/IR/CFG.h>`, `#include <llvm/IR/Dominators.h>`, `#include <set>`, `#include <vector>`. In the anonymous namespace, before `encodeQuery`:

```cpp
        using BlockEdge = std::pair<const llvm::BasicBlock*, const llvm::BasicBlock*>;

        /// Back edges of @p function, or std::nullopt when one enters a cycle through a block
        /// that does not dominate its source (irreducible control flow).
        static std::optional<std::set<BlockEdge>>
        reducibleBackEdges(const llvm::Function& function, const llvm::DominatorTree& dominators)
        {
            llvm::SmallVector<BlockEdge, 8> edges;
            llvm::FindFunctionBackedges(function, edges);
            std::set<BlockEdge> out;
            for (const BlockEdge& edge : edges)
            {
                if (!dominators.dominates(edge.second, edge.first))
                    return std::nullopt;
                out.insert(edge);
            }
            return out;
        }

        /// Dominators of @p block, nearest first, @p block excluded.
        static std::vector<const llvm::BasicBlock*>
        strictDominators(const llvm::BasicBlock& block, const llvm::DominatorTree& dominators)
        {
            std::vector<const llvm::BasicBlock*> out;
            const llvm::DomTreeNode* node = dominators.getNode(&block);
            for (node = node ? node->getIDom() : nullptr; node; node = node->getIDom())
                out.push_back(node->getBlock());
            return out;
        }

        /// Condition under which control flows from @p from to @p to; std::nullopt means
        /// always (unconditional edge, or a condition the encoder cannot translate).
        static std::optional<ExprId> encodeEdgeCondition(const llvm::BasicBlock& from,
                                                         const llvm::BasicBlock& to,
                                                         ConstraintIrBuilder& builder,
                                                         LlvmExprEncoder& exprEncoder)
        {
            const llvm::Instruction* terminator = from.getTerminator();
            if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(terminator))
            {
                if (!branch->isConditional() ||
                    branch->getSuccessor(0) == branch->getSuccessor(1))
                    return std::nullopt;
                const std::optional<ExprId> condition =
                    exprEncoder.encodeAsBoolean(branch->getCondition());
                if (!condition)
                    return std::nullopt;
                if (branch->getSuccessor(0) == &to)
                    return condition;
                return builder.makeUnary(ExprKind::Not, *condition, 1);
            }

            const auto* switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator);
            if (!switchInst)
                return std::nullopt;
            const std::optional<ExprId> selector = exprEncoder.encodeValue(switchInst->getCondition());
            if (!selector)
                return std::nullopt;
            const std::uint32_t bitWidth = builder.node(*selector).bitWidth;
            if (bitWidth > 64)
                return std::nullopt;

            const bool isDefault = switchInst->getDefaultDest() == &to;
            std::optional<ExprId> taken;     // a case leading to `to` matches
            std::optional<ExprId> noneMatch; // no case matches: the default is taken
            for (const auto& caseHandle : switchInst->cases())
            {
                const ExprId value =
                    builder.makeConstant(caseHandle.getCaseValue()->getSExtValue(), bitWidth);
                if (caseHandle.getCaseSuccessor() == &to)
                {
                    const ExprId equal = builder.makeBinary(ExprKind::Eq, *selector, value, 1);
                    taken = taken ? builder.makeBinary(ExprKind::Or, *taken, equal, 1) : equal;
                }
                if (isDefault)
                {
                    const ExprId differ = builder.makeBinary(ExprKind::Ne, *selector, value, 1);
                    noneMatch =
                        noneMatch ? builder.makeBinary(ExprKind::And, *noneMatch, differ, 1) : differ;
                }
            }
            if (isDefault)
            {
                if (!noneMatch)
                    return std::nullopt;
                return taken ? builder.makeBinary(ExprKind::Or, *taken, *noneMatch, 1) : *noneMatch;
            }
            return taken;
        }

        /// Condition under which @p target is reached from @p head, one of its dominators, over
        /// the CFG without @p backEdges. std::nullopt means always. Booleans only: a constant
        /// would be a bitvector for the backends, so "always" is the absence of a node.
        static std::optional<ExprId> encodeReachCondition(const llvm::BasicBlock& head,
                                                          const llvm::BasicBlock& target,
                                                          const std::set<BlockEdge>& backEdges,
                                                          ConstraintIrBuilder& builder,
                                                          LlvmExprEncoder& exprEncoder)
        {
            const auto forward = [&](const llvm::BasicBlock* from, const llvm::BasicBlock* to)
            { return !backEdges.contains({from, to}); };

            // Region: blocks on a forward path from `head` to `target`.
            std::set<const llvm::BasicBlock*> reachable{&head};
            std::vector<const llvm::BasicBlock*> work{&head};
            while (!work.empty())
            {
                const llvm::BasicBlock* block = work.back();
                work.pop_back();
                for (const llvm::BasicBlock* succ : llvm::successors(block))
                {
                    if (forward(block, succ) && reachable.insert(succ).second)
                        work.push_back(succ);
                }
            }
            std::set<const llvm::BasicBlock*> region{&target};
            work = {&target};
            while (!work.empty())
            {
                const llvm::BasicBlock* block = work.back();
                work.pop_back();
                if (block == &head)
                    continue;
                for (const llvm::BasicBlock* pred : llvm::predecessors(block))
                {
                    if (forward(pred, block) && reachable.contains(pred) &&
                        region.insert(pred).second)
                        work.push_back(pred);
                }
            }

            // Reverse post-order puts the source of every forward edge before its target.
            std::map<const llvm::BasicBlock*, std::optional<ExprId>> reach;
            const llvm::ReversePostOrderTraversal<const llvm::Function*> order(head.getParent());
            for (const llvm::BasicBlock* block : order)
            {
                if (!region.contains(block))
                    continue;
                if (block == &head)
                {
                    reach[block] = std::nullopt;
                    continue;
                }
                std::optional<ExprId> any;
                bool always = false;
                std::set<const llvm::BasicBlock*> seen;
                for (const llvm::BasicBlock* pred : llvm::predecessors(block))
                {
                    if (!region.contains(pred) || !forward(pred, block) ||
                        !seen.insert(pred).second)
                        continue;
                    std::optional<ExprId> term = reach.at(pred);
                    if (const std::optional<ExprId> edge =
                            encodeEdgeCondition(*pred, *block, builder, exprEncoder))
                        term = term ? builder.makeBinary(ExprKind::And, *term, *edge, 1) : *edge;
                    if (!term)
                        always = true;
                    else
                        any = any ? builder.makeBinary(ExprKind::Or, *any, *term, 1) : *term;
                }
                reach[block] = always ? std::nullopt : any;
            }
            return reach.contains(&target) ? reach.at(&target) : std::nullopt;
        }
```

Replace `encodeQuery` with:

```cpp
        /// Builds the query at @p point: the rule's ranges, the reachability condition of
        /// @p point from the farthest dominator that keeps the query within the node budget,
        /// then what @p postEncode asserts. Without facts, in the entry block, in an
        /// irreducible function, or when even the immediate dominator is over budget, the
        /// query has no path condition.
        static ConstraintIR encodeQuery(const std::map<const llvm::Value*, IntRange>& ranges,
                                        const QueryPoint& point,
                                        const QueryPostEncoder& postEncode)
        {
            const auto build = [&](const llvm::BasicBlock* head,
                                   const std::set<BlockEdge>* backEdges)
            {
                ConstraintIR ir;
                ir.intervals.reserve(ranges.size());
                ConstraintIrBuilder builder(ir);
                LlvmExprEncoder exprEncoder(builder, nullptr, point.facts);
                encodeRangeAssertions(ranges, ir, builder, exprEncoder);
                if (head)
                {
                    if (const std::optional<ExprId> reach = encodeReachCondition(
                            *head, *point.inst->getParent(), *backEdges, builder, exprEncoder))
                        builder.addAssertion(*reach);
                }
                postEncode(builder, exprEncoder);
                return ir;
            };

            if (!point.facts || !point.inst)
                return build(nullptr, nullptr);

            const llvm::DominatorTree& dominators = point.facts->dominatorTree();
            const std::optional<std::set<BlockEdge>> backEdges =
                reducibleBackEdges(*point.inst->getFunction(), dominators);
            const std::vector<const llvm::BasicBlock*> heads =
                strictDominators(*point.inst->getParent(), dominators);
            if (!backEdges || heads.empty())
                return build(nullptr, nullptr);
            if (point.budgetNodes == 0)
                return build(heads.back(), &*backEdges);

            // A farther dominator heads a larger region, so the heads that fit form a prefix of
            // `heads`: binary search for its last element.
            std::optional<ConstraintIR> best;
            std::size_t low = 0;
            std::size_t high = heads.size();
            while (low < high)
            {
                const std::size_t middle = low + (high - low) / 2;
                ConstraintIR candidate = build(heads[middle], &*backEdges);
                if (candidate.nodes.size() <= point.budgetNodes)
                {
                    best = std::move(candidate);
                    low = middle + 1;
                }
                else
                {
                    high = middle;
                }
            }
            return best ? std::move(*best) : build(nullptr, nullptr);
        }
```

- [ ] **Step 5: Build, format, run**

Format `src/analysis/smt/SmtEncoding.cpp` and the test file, build all targets, run `./build/stack_usage_analyzer_unit_tests . | grep -E "SMT (path condition|memory model)"`.
Expected: all `[PASS]`.

- [ ] **Step 6: Full suite and commit**

Run the unit tests and `python3 run_test.py --jobs 8 --no-cache`. Expected: all green (the rules still pass no facts).

```bash
git add src/analysis/smt/SmtEncoding.cpp test/unit/smt_path_input.c test/unit/analyzer_module_unit_tests.cpp
git commit -m "feat(smt): add the reachability condition of the query point" -m "Refs #A2_ISSUE"
```

### Task 15 (A2.5): Wire the four rules, end-to-end fixtures

**Files:**
- Modify: `include/analysis/smt/SmtRefinement.hpp`
- Modify: `src/analysis/StackBufferAnalysis.cpp`, `src/analysis/OOBReadAnalysis.cpp`, `src/analysis/SizeMinusKWrites.cpp`, `src/analysis/IntegerOverflowAnalysis.cpp`
- Create: `test/integer-overflow/smt-path-cert-guarded-add.c`, `test/integer-overflow/smt-path-guarded-alloc-size.c`, `test/integer-overflow/smt-path-broken-overflow-check-is-kept.c`, `test/bound-storage/smt-path-relational-index-guard.c`, `test/bound-storage/smt-path-loop-counter-is-kept.c`

**Interfaces:**
- Consumes: `QueryPoint` (Task A2.3), path condition (Task A2.4).
- Produces: `QueryPoint SmtConstraintEvaluator::queryPoint(const llvm::Instruction* inst, const FunctionFacts* facts) const` (protected); every evaluator method of the four rules takes a trailing `const FunctionFacts* facts`.

- [ ] **Step 1: Write the fixtures**

`test/integer-overflow/smt-path-cert-guarded-add.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: the CERT INT32-C precondition makes `a + b` safe.
//
// The guard is a disjunction compiled to several branches, and it relates `a` to `b`, so no
// interval on either operand proves the addition safe. The solver proves it from the
// reachability condition of the return, which it only sees once loads of the same slot are
// tied together.

#include <limits.h>

int checked_add(int a, int b)
{
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b))
        return -1;
    return a + b;
}

// [default] at line 16, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
```

`test/integer-overflow/smt-path-guarded-alloc-size.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: the guard on `n` makes `n * 8` unable to wrap before `malloc`.

#include <stdint.h>
#include <stdlib.h>

void* allocate_words(size_t n)
{
    if (n > SIZE_MAX / 8)
        return 0;
    return malloc(n * 8);
}

// [default] at line 12, column 12
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// [smt-z3] not contains: potential integer overflow in size computation
```

`test/integer-overflow/smt-path-broken-overflow-check-is-kept.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// Guard: path conditions use the wrapping arithmetic that -O0 code executes.
//
// `a + 1 < a` only holds when the addition wraps (a == INT_MAX), and then `a + 2` overflows.
// Assuming `nsw` on the condition would make the branch look unreachable and hide the report.
//
// This fixture must report in BOTH passes.

int broken_check(int a)
{
    if (a + 1 < a)
        return a + 2;
    return 0;
}

// strict-expectation-details: true

// at line 13, column 18
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: add
// ↳ result is returned without a provable non-overflow bound
```

`test/bound-storage/smt-path-relational-index-guard.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: `i < n` and `n <= 16` together keep `i` inside `buf`.
//
// The relation `i < n` is established before `n` is bounded, so the interval of `i` at the
// access only knows `i <= 20` and the default pass reports a possible overflow. The solver
// combines the three guards and proves the access in bounds.

void relational_guard(int i, int n)
{
    char buf[16];
    if (i > 20)
        return;
    if (i >= n)
        return;
    if (n > 16)
        return;
    buf[i] = 1;
}

// [default] at line 18, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 20 (array last valid index: 15)
// ↳ (this is a write access)

// [smt-z3] not contains: potential stack buffer overflow on variable 'buf'
```

`test/bound-storage/smt-path-loop-counter-is-kept.c`:

```c
// SPDX-License-Identifier: Apache-2.0
//
// Guard: a load inside a loop is not the value stored before the loop.
//
// Inside the loop, `i` is read through the MemoryPhi of the loop header, so it is only bounded
// by the loop guard `i < 20`, and `buf[19]` is out of bounds. Tying that load to the initial
// store `i = 0` would prove the access safe and hide the report.
//
// This fixture must report in BOTH passes.

void loop_counter(void)
{
    char buf[16];
    int i = 0;
    while (i < 20)
    {
        buf[i] = 0;
        i++;
    }
}

// at line 17, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 19 (array last valid index: 15)
// ↳ (this is a write access)
```

- [ ] **Step 2: Run them to see the three false positives fail**

Run: `PYTHONPATH=. python3 "$SCRATCH/smtimprove/check_fixtures.py" test/integer-overflow/smt-path-*.c test/bound-storage/smt-path-*.c`
Expected: `smt-path-cert-guarded-add.c`, `smt-path-guarded-alloc-size.c`, `smt-path-relational-index-guard.c` fail in `[pass: smt-z3]`; the two `-is-kept` fixtures pass.

- [ ] **Step 3: Add `queryPoint` to the evaluator**

`include/analysis/smt/SmtRefinement.hpp`: add `#include "analysis/smt/SmtEncoding.hpp"`, and in the `protected:` section, after `evaluateQuery`:

```cpp
        /// @brief Query point for @p inst, with this rule's node budget.
        [[nodiscard]] QueryPoint queryPoint(const llvm::Instruction* inst,
                                            const FunctionFacts* facts) const
        {
            return QueryPoint{.inst = inst, .facts = facts, .budgetNodes = budgetNodes_};
        }
```

- [ ] **Step 4: Pass the facts through every evaluator**

For each evaluator method of Task A1.3 Step 4 except the recursion one: add a trailing parameter `const FunctionFacts* facts`, and in its lambda replace `smt::QueryPoint{.inst = contextInst}` with `queryPoint(contextInst, facts)`. Then update the callers:

`src/analysis/StackBufferAnalysis.cpp`: give `isUpperViolationInfeasibleBySmt` and `isLowerViolationInfeasibleBySmt` a trailing `const FunctionFacts& facts` parameter and pass `&facts` to the evaluator; in `analyzeStackBufferOverflowsInFunction`, append `facts` to their four calls:

```cpp
                                if (isUpperViolationInfeasibleBySmt(evaluator, R, baseIdxVal,
                                                                    arraySize, *S, facts))
```
(and the same for `*L`, and for the two `isLowerViolationInfeasibleBySmt(evaluator, R, baseIdxVal, *S|*L, facts)` calls).

`src/analysis/OOBReadAnalysis.cpp`: give `isHeapIndexViolationInfeasibleBySmt` a trailing `const FunctionFacts& facts`, pass `&facts` to both evaluator calls, and call it with:

```cpp
                        if (isHeapIndexViolationInfeasibleBySmt(evaluator, pointRanges.at(inst),
                                                                queryIndex, capacity, inst, facts))
```

`src/analysis/SizeMinusKWrites.cpp`:

```cpp
                    if (evaluator.isSignedLessEqualFeasible(queryRanges, *sizeBase, k, at,
                                                            &facts) == SmtFeasibility::Infeasible)
                        issue.sizeAboveK = true;
```

`src/analysis/IntegerOverflowAnalysis.cpp`: pass `&facts` in the signed-arithmetic call:

```cpp
                            if (evaluator.isSignedOverflowFeasible(queryRanges, *binary, &inst,
                                                                   &facts) ==
                                SmtFeasibility::Infeasible)
```

give `shouldSuppressRiskWithSmt` a trailing `const FunctionFacts& facts`, pass `&facts` to its four evaluator calls, and call it with `shouldSuppressRiskWithSmt(evaluator, ranges, *risk, inst, facts)`.

- [ ] **Step 5: Build, format, run the fixtures**

Format the modified files, build all targets, run the Step 2 command.
Expected: all five fixtures PASS in both passes.

- [ ] **Step 6: Full suite**

Run the unit tests and `python3 run_test.py --jobs 8 --no-cache`.
Expected: all green. If an existing fixture fails in `[pass: smt-z3]`, stop and ask the user as in Task A1.5 Step 5.

- [ ] **Step 7: Commit**

```bash
git add include/analysis/smt/SmtRefinement.hpp src/analysis/StackBufferAnalysis.cpp src/analysis/OOBReadAnalysis.cpp src/analysis/SizeMinusKWrites.cpp src/analysis/IntegerOverflowAnalysis.cpp test/integer-overflow/smt-path-cert-guarded-add.c test/integer-overflow/smt-path-guarded-alloc-size.c test/integer-overflow/smt-path-broken-overflow-check-is-kept.c test/bound-storage/smt-path-relational-index-guard.c test/bound-storage/smt-path-loop-counter-is-kept.c
git commit -m "feat(smt): give the value rules path-sensitive queries" -m "Refs #A2_ISSUE"
```

### Task 16 (A2.6): Documentation, validation, report, PR

**Files:**
- Modify: `docs/architecture/smt-solver-integration.md` (section "Current Implementation Status (March 2026)")

- [ ] **Step 1: Update the status section**

Rename the heading to `## Current Implementation Status (September 2026)`, add to "Implemented in codebase":

```markdown
7. Path-sensitive queries for integer-overflow, size-minus-k, stack-buffer and oob-read: the
   reachability condition of the query point over the CFG without back edges, from the farthest
   dominator within the node budget; loads encoded through their MemorySSA clobber
   (`FunctionFacts::clobberingAccess`); wrapping semantics, no `nsw`/`nuw` assumption.
8. Queries are built only when SMT is on for the rule, and asked even when no range is known.
9. CI builds and exercises the Z3 backend.
```

and replace "Planned next" item 1 with:

```markdown
1. Recursion still encodes interval-derived constraints only; path-sensitive base-case queries
   would make `unsat` add diagnostics, which is a separate decision.
```

- [ ] **Step 2: Full validation** — build, unit tests, `python3 run_test.py --jobs 8 --no-cache`, the clang-format check of Task A0.4 Step 1. Expected: all green.
- [ ] **Step 3: Measure** — run Procedure M. Expected: SMT off identical diagnostics and time within noise; every removed diagnostic labelled; no true positive removed.
- [ ] **Step 4: Commit the documentation**

```bash
git add docs/architecture/smt-solver-integration.md
git commit -m "docs(smt): record path-sensitive queries in the integration status" -m "Refs #A2_ISSUE"
```

- [ ] **Step 5: Push and open the PR** — push, then `gh pr create --base main --title "feat(smt): path conditions over a MemorySSA memory model"` with a body in the format of Task A0.4 (`Closes #A2_ISSUE`, links, `What`, `Regression report` from Procedure M).
- [ ] **Step 6: Stop** — report to the user; decision B (detector, counterexamples, recursion) is taken with these measurements.
