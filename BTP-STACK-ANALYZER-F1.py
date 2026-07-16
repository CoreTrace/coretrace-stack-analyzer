#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for stack usage analysis features.

This script reuses repository fixtures and validates stack_usage_analyzer JSON
fields for:
  - static stack size and call-chain propagation,
  - VLA and alloca dynamic stack usage,
  - unknown stack propagation,
  - infinite recursion,
  - stack pointer escapes.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_ANALYZER = Path(os.environ.get("CORETRACE_STACK_ANALYZER", "./build/stack_usage_analyzer"))
ANALYZER_MODES = ("ir", "abi")
GREEN = "\033[32m"
RED = "\033[31m"
PURPLE = "\033[35m"
RESET = "\033[0m"
SEPARATOR = "--------"


def feature_reference_from_filename(path: Path) -> str:
    marker = "-F"
    if marker not in path.stem:
        return "[F?]"
    suffix = path.stem.rsplit(marker, 1)[1]
    digits = "".join(char for char in suffix if char.isdigit())
    return f"[F{digits}]" if digits else "[F?]"


FEATURE_REF = feature_reference_from_filename(Path(__file__))


def print_log(*args: object, **kwargs: Any) -> None:
    print(FEATURE_REF, *args, **kwargs)


@dataclass(frozen=True)
class FixtureSet:
    static_stack: Path = Path("test/local-storage/c/stack-callee-caller.c")
    vla_unknown: Path = Path("test/vla/vla-unknown-stack.c")
    alloca_dynamic: Path = Path("test/alloca/user-controlled.c")
    infinite_recursion: Path = Path("test/recursion/c/infinite-recursion.c")
    stack_escape: Path = Path("test/escape-stack/return-buf.c")


@dataclass(frozen=True)
class AnalyzerResult:
    fixture: Path
    requested_mode: str
    command: tuple[str, ...]
    payload: dict[str, Any]


@dataclass(frozen=True)
class ProofCheck:
    name: str
    found: bool
    detail: str


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prove stack usage analysis features using existing test fixtures."
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help="Path to stack_usage_analyzer, or CORETRACE_STACK_ANALYZER.",
    )
    parser.add_argument("--static-stack-fixture", type=Path, default=FixtureSet.static_stack)
    parser.add_argument("--vla-fixture", type=Path, default=FixtureSet.vla_unknown)
    parser.add_argument("--alloca-fixture", type=Path, default=FixtureSet.alloca_dynamic)
    parser.add_argument("--recursion-fixture", type=Path, default=FixtureSet.infinite_recursion)
    parser.add_argument("--escape-fixture", type=Path, default=FixtureSet.stack_escape)
    return parser.parse_args(argv)


def run_analyzer(analyzer: Path, fixture: Path, mode: str) -> AnalyzerResult:
    if not analyzer.exists():
        raise FileNotFoundError(f"Analyzer not found: {analyzer}")
    if not fixture.exists():
        raise FileNotFoundError(f"Fixture not found: {fixture}")

    command = (str(analyzer), str(fixture), f"--mode={mode}", "--format=json")
    result = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        raise RuntimeError(
            f"Analyzer failed for {fixture} with exit code {result.returncode}\n"
            f"stdout:\n{result.stdout}\n"
            f"stderr:\n{result.stderr}"
        )

    payload = json.loads(result.stdout)
    if not isinstance(payload, dict):
        raise ValueError(f"Analyzer output for {fixture} is not a JSON object.")
    return AnalyzerResult(fixture=fixture, requested_mode=mode, command=command, payload=payload)


def reported_mode(payload: dict[str, Any]) -> str:
    meta = payload.get("meta")
    if not isinstance(meta, dict):
        return "<missing>"
    mode = meta.get("mode")
    return mode if isinstance(mode, str) else "<missing>"


def functions_by_name(payload: dict[str, Any]) -> dict[str, dict[str, Any]]:
    functions = payload.get("functions")
    if not isinstance(functions, list):
        raise ValueError("JSON report has no functions array.")
    return {
        item["name"]: item
        for item in functions
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }


def diagnostics(payload: dict[str, Any]) -> list[dict[str, Any]]:
    raw = payload.get("diagnostics")
    if not isinstance(raw, list):
        raise ValueError("JSON report has no diagnostics array.")
    return [item for item in raw if isinstance(item, dict)]


def message_of(diag: dict[str, Any]) -> str:
    details = diag.get("details")
    if not isinstance(details, dict):
        return ""
    message = details.get("message")
    return message if isinstance(message, str) else ""


def has_diag(
    payload: dict[str, Any],
    *,
    rule_id: str | None = None,
    severity: str | None = None,
    function: str | None = None,
    message_contains: tuple[str, ...] = (),
) -> bool:
    for diag in diagnostics(payload):
        if rule_id is not None and diag.get("ruleId") != rule_id:
            continue
        if severity is not None and diag.get("severity") != severity:
            continue
        location = diag.get("location")
        if function is not None:
            if not isinstance(location, dict) or location.get("function") != function:
                continue
        message = message_of(diag)
        if all(needle in message for needle in message_contains):
            return True
    return False


def colored(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def status_label(found: bool) -> str:
    return colored("PASS", GREEN) if found else colored("NONE", RED)


def require(condition: bool, proof: str, detail: str) -> ProofCheck:
    return ProofCheck(name=proof, found=condition, detail=detail)


def print_report(result: AnalyzerResult, checks: list[ProofCheck]) -> None:
    print_log(SEPARATOR)
    print_log(colored(str(result.fixture), PURPLE))
    print_log(f"mode: {reported_mode(result.payload)}")
    for check in checks:
        print_log(f"- {check.name}: {status_label(check.found)}")
        if not check.found:
            print_log(f"  detail: {check.detail}")
    print_log(f"command: {' '.join(result.command)}")


def prove_static_stack(result: AnalyzerResult) -> list[ProofCheck]:
    funcs = functions_by_name(result.payload)
    foo = funcs.get("foo", {})
    bar = funcs.get("bar", {})
    mano = funcs.get("mano", {})
    foo_max = foo.get("maxStack")
    bar_max = bar.get("maxStack")
    mano_max = mano.get("maxStack")
    return [
        require(
            foo.get("localStack") == 8192000000
            and foo.get("maxStack") == 8192000000
            and foo.get("exceedsLimit") is True,
            "static stack size",
            "expected foo to expose a fixed oversized stack frame",
        ),
        require(
            isinstance(foo_max, int)
            and isinstance(bar_max, int)
            and isinstance(mano_max, int)
            and bar_max >= foo_max
            and mano_max >= bar_max
            and bar.get("exceedsLimit") is True
            and mano.get("exceedsLimit") is True,
            "static stack propagation",
            "expected bar/mano to propagate foo's oversized stack through the call graph",
        ),
        require(
            has_diag(
                result.payload,
                rule_id="StackFrameTooLarge",
                severity="ERROR",
                function="mano",
                message_contains=("path: mano -> bar -> foo",),
            ),
            "stack overflow path diagnostic",
            "expected call-chain diagnostic mano -> bar -> foo",
        ),
    ]


def prove_vla_unknown(result: AnalyzerResult) -> list[ProofCheck]:
    funcs = functions_by_name(result.payload)
    consume = funcs.get("consume", {})
    main = funcs.get("main", {})
    main_lower_bound = main.get("maxStackLowerBound")
    return [
        require(
            consume.get("hasDynamicAlloca") is True
            and consume.get("localStackUnknown") is True
            and consume.get("maxStackUnknown") is True
            and consume.get("localStackLowerBound") == 32,
            "VLA dynamic stack size",
            "expected consume to have unknown local/max stack with a lower bound",
        ),
        require(
            main.get("localStackUnknown") is False
            and main.get("maxStackUnknown") is True
            and isinstance(main_lower_bound, int)
            and main_lower_bound >= 48,
            "unknown stack propagation",
            "expected main to propagate unknown stack usage from consume",
        ),
        require(
            has_diag(
                result.payload,
                rule_id="VLAUsage",
                severity="WARNING",
                function="consume",
                message_contains=("dynamic stack allocation", "VLA"),
            ),
            "VLA diagnostic",
            "expected VLAUsage warning in consume",
        ),
        require(
            has_diag(
                result.payload,
                rule_id="AllocaUserControlled",
                severity="WARNING",
                function="consume",
                message_contains=("user-controlled alloca size",),
            ),
            "user-controlled VLA diagnostic",
            "expected AllocaUserControlled warning in consume",
        ),
    ]


def prove_alloca(result: AnalyzerResult) -> list[ProofCheck]:
    funcs = functions_by_name(result.payload)
    foo = funcs.get("foo", {})
    return [
        require(
            foo.get("hasDynamicAlloca") is True
            and foo.get("localStackUnknown") is True
            and foo.get("maxStackUnknown") is True,
            "alloca dynamic stack size",
            "expected foo to expose dynamic alloca as unknown stack usage",
        ),
        require(
            has_diag(
                result.payload,
                rule_id="AllocaUserControlled",
                severity="WARNING",
                function="foo",
                message_contains=("variable 'buf'", "stack usage grows with runtime value"),
            ),
            "alloca diagnostic",
            "expected user-controlled alloca warning for buf",
        ),
    ]


def prove_infinite_recursion(result: AnalyzerResult) -> list[ProofCheck]:
    funcs = functions_by_name(result.payload)
    tutu = funcs.get("tutu", {})
    return [
        require(
            tutu.get("isRecursive") is True and tutu.get("hasInfiniteSelfRecursion") is True,
            "infinite recursion flags",
            "expected tutu to be recursive with unconditional self-recursion",
        ),
        require(
            has_diag(
                result.payload,
                severity="ERROR",
                function="tutu",
                message_contains=("unconditional self recursion", "overflow the stack"),
            ),
            "infinite recursion diagnostic",
            "expected error diagnostic for unconditional self recursion",
        ),
    ]


def prove_stack_escape(result: AnalyzerResult) -> list[ProofCheck]:
    return [
        require(
            has_diag(
                result.payload,
                rule_id="StackPointerEscape",
                severity="WARNING",
                function="ret_buf",
                message_contains=("address of variable 'buf' escapes", "return statement"),
            ),
            "stack pointer escape diagnostic",
            "expected StackPointerEscape warning for returning buf",
        )
    ]


def prove_mode(result: AnalyzerResult) -> list[ProofCheck]:
    expected = result.requested_mode.upper()
    observed = reported_mode(result.payload)
    return [
        require(
            observed == expected,
            f"analyzer mode {expected}",
            f"expected JSON meta.mode={expected}, got {observed}",
        )
    ]


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        fixtures = FixtureSet(
            static_stack=args.static_stack_fixture,
            vla_unknown=args.vla_fixture,
            alloca_dynamic=args.alloca_fixture,
            infinite_recursion=args.recursion_fixture,
            stack_escape=args.escape_fixture,
        )
        reports: list[tuple[AnalyzerResult, list[ProofCheck]]] = []
        for mode in ANALYZER_MODES:
            static = run_analyzer(args.analyzer, fixtures.static_stack, mode)
            vla = run_analyzer(args.analyzer, fixtures.vla_unknown, mode)
            alloca = run_analyzer(args.analyzer, fixtures.alloca_dynamic, mode)
            recursion = run_analyzer(args.analyzer, fixtures.infinite_recursion, mode)
            escape = run_analyzer(args.analyzer, fixtures.stack_escape, mode)
            reports.extend(
                [
                    (static, prove_mode(static) + prove_static_stack(static)),
                    (vla, prove_mode(vla) + prove_vla_unknown(vla)),
                    (alloca, prove_mode(alloca) + prove_alloca(alloca)),
                    (recursion, prove_mode(recursion) + prove_infinite_recursion(recursion)),
                    (escape, prove_mode(escape) + prove_stack_escape(escape)),
                ]
            )
        all_checks = [check for _, checks in reports for check in checks]
        all_found = all(check.found for check in all_checks)

        print_log(f"BTP-STACK-ANALYZER-F1: {status_label(all_found)}")
        for result, checks in reports:
            print_report(result, checks)
        return 0 if all_found else 1
    except Exception as exc:
        print_log(f"BTP-STACK-ANALYZER-F1: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
