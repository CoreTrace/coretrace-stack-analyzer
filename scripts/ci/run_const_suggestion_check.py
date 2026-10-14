#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the tests of the experimental check of const suggestions (--experimental-const-suggestion-check).

The tests come from run_test.py: the const-suggestion fixtures, which enable the option with their
`// analyzer-arg:` line, and the checks that pass it. Together they expect suggestions that are
kept once checked, suggestions that are removed, and suggestions kept as not checked; this script
fails if the fixtures stop expecting any of the three.

These tests cover only this repository's fixtures, with their compile commands, on the toolchain of
the machine that runs them: they say nothing of other projects or configurations, nor of analyses
run without the option.

usage: run_const_suggestion_check.py   (after a build: uses build/stack_usage_analyzer)
"""
from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test" / "pointer_reference-const_correctness"
CHECKS = (
    "check_const_suggestion_cross_tu",
    "check_const_suggestion_without_source",
    "check_const_suggestion_compilation_database",
    "check_const_suggestion_check_is_opt_in",
)
# What the fixtures expect, by the expectation that says it.
OUTCOMES = {
    "kept, checked": "// ↳ checked in ",
    "removed": "// not contains: ",
    "kept, not checked": "// ↳ not checked with Clang: ",
}


def load_run_test():
    spec = importlib.util.spec_from_file_location("run_test", ROOT / "run_test.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["run_test"] = module
    spec.loader.exec_module(module)
    return module


def main() -> int:
    fixtures = sorted(p for p in FIXTURES.glob("const-suggestion-*") if p.suffix in (".c", ".cpp"))
    counts = {outcome: 0 for outcome in OUTCOMES}
    for fixture in fixtures:
        for line in fixture.read_text(encoding="utf-8").splitlines():
            for outcome, marker in OUTCOMES.items():
                if line.strip().startswith(marker):
                    counts[outcome] += 1
    print(f"{len(fixtures)} fixtures; expectations: "
          + ", ".join(f"{outcome} {n}" for outcome, n in counts.items()))
    missing = [outcome for outcome, n in counts.items() if n == 0]
    if missing:
        print(f"::error::the fixtures no longer expect: {', '.join(missing)}")
        return 1

    os.chdir(ROOT)
    run_test = load_run_test()
    # The fixtures' results depend on headers that the cache key of run_test.py does not cover.
    run_test.RUN_CONFIG.cache_enabled = False
    ok = True
    for fixture in fixtures:
        passed, total, matched, report = run_test.check_file(fixture.relative_to(ROOT))
        print(f"{'PASS' if passed else 'FAIL'} {fixture.name}: {matched}/{total}")
        if not passed:
            print(report)
        ok = ok and passed
    for name in CHECKS:
        passed = getattr(run_test, name)()
        print(f"{'PASS' if passed else 'FAIL'} {name}")
        ok = ok and passed
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
