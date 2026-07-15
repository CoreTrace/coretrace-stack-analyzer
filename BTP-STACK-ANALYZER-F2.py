#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for memory intrinsic, aliasing, and const-correctness features.

The script validates stack_usage_analyzer JSON diagnostics over existing
fixtures for:
  - memcpy overflow,
  - memset overflow,
  - deep aliasing through stack-buffer bounds,
  - basic pointer const-correctness suggestions.
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
    memcpy_overflow: Path = Path("test/cpy-buffer/bad-usage-memcpy.c")
    memset_overflow: Path = Path("test/cpy-buffer/bad-usage-memset.c")
    deep_aliasing: Path = Path("test/bound-storage/deep-alias.c")
    const_correctness: Path = Path("test/pointer_reference-const_correctness/readonly-pointer.c")


@dataclass(frozen=True)
class AnalyzerResult:
    fixture: Path
    command: tuple[str, ...]
    payload: dict[str, Any]


@dataclass(frozen=True)
class ProofCheck:
    name: str
    found: bool
    detail: str


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prove memcpy/memset overflow, deep aliasing, and const-correctness "
            "features using existing test fixtures."
        )
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help="Path to stack_usage_analyzer, or CORETRACE_STACK_ANALYZER.",
    )
    parser.add_argument("--memcpy-fixture", type=Path, default=FixtureSet.memcpy_overflow)
    parser.add_argument("--memset-fixture", type=Path, default=FixtureSet.memset_overflow)
    parser.add_argument("--deep-alias-fixture", type=Path, default=FixtureSet.deep_aliasing)
    parser.add_argument("--const-fixture", type=Path, default=FixtureSet.const_correctness)
    return parser.parse_args(argv)


def run_analyzer(analyzer: Path, fixture: Path) -> AnalyzerResult:
    if not analyzer.exists():
        raise FileNotFoundError(f"Analyzer not found: {analyzer}")
    if not fixture.exists():
        raise FileNotFoundError(f"Fixture not found: {fixture}")

    command = (str(analyzer), str(fixture), "--format=json")
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
    return AnalyzerResult(fixture=fixture, command=command, payload=payload)


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


def variable_aliasing(diag: dict[str, Any]) -> list[str]:
    details = diag.get("details")
    if not isinstance(details, dict):
        return []
    aliases = details.get("variableAliasing")
    return [item for item in aliases if isinstance(item, str)] if isinstance(aliases, list) else []


def has_diag(
    payload: dict[str, Any],
    *,
    rule_id: str | None = None,
    severity: str | None = None,
    function: str | None = None,
    message_contains: tuple[str, ...] = (),
    aliases_contain: tuple[str, ...] = (),
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
        aliases = variable_aliasing(diag)
        if all(needle in message for needle in message_contains) and all(
            alias in aliases for alias in aliases_contain
        ):
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
    for check in checks:
        print_log(f"- {check.name}: {status_label(check.found)}")
        if not check.found:
            print_log(f"  detail: {check.detail}")
    print_log(f"command: {' '.join(result.command)}")


def prove_memcpy(result: AnalyzerResult) -> list[ProofCheck]:
    return [
        require(
            has_diag(
                result.payload,
                severity="WARNING",
                function="foo",
                message_contains=(
                    "potential stack buffer overflow in memcpy",
                    "variable 'buf'",
                    "destination stack buffer size: 10 bytes",
                    "requested 20 bytes",
                ),
            ),
            "memcpy overflow",
            "expected memcpy overflow warning for buf[10] with 20 requested bytes",
        )
    ]


def prove_memset(result: AnalyzerResult) -> list[ProofCheck]:
    return [
        require(
            has_diag(
                result.payload,
                severity="WARNING",
                function="foo",
                message_contains=(
                    "potential stack buffer overflow in memset",
                    "variable 'buf'",
                    "destination stack buffer size: 10 bytes",
                    "requested 100 bytes",
                ),
            ),
            "memset overflow",
            "expected memset overflow warning for buf[10] with 100 requested bytes",
        )
    ]


def prove_deep_aliasing(result: AnalyzerResult) -> list[ProofCheck]:
    return [
        require(
            has_diag(
                result.payload,
                rule_id="StackBufferOverflow",
                severity="WARNING",
                function="deep_alias",
                message_contains=(
                    "potential stack buffer overflow on variable 'buf'",
                    "alias path: buf -> arraydecay -> p1 -> p2 -> pp",
                    "array last valid index: 9",
                ),
                aliases_contain=("buf", "arraydecay", "p1", "p2", "pp"),
            ),
            "deep aliasing",
            "expected stack-buffer overflow through buf -> arraydecay -> p1 -> p2 -> pp",
        )
    ]


def prove_const_correctness(result: AnalyzerResult) -> list[ProofCheck]:
    return [
        require(
            has_diag(
                result.payload,
                rule_id="ConstParameterNotModified.Pointer",
                severity="INFO",
                function="myfunc",
                message_contains=(
                    "parameter 'param3'",
                    "current type: int32_t *param3",
                    "suggested type: const int32_t *param3",
                ),
            ),
            "const-correctness pointer",
            "expected const suggestion for param3",
        ),
        require(
            has_diag(
                result.payload,
                rule_id="ConstParameterNotModified.PointerConstOnly",
                severity="INFO",
                function="myfunc",
                message_contains=(
                    "parameter 'param4'",
                    "current type: int32_t * const param4",
                    "suggested type: const int32_t *param4",
                ),
            ),
            "const-correctness pointer const-only",
            "expected const suggestion for param4",
        ),
    ]


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        fixtures = FixtureSet(
            memcpy_overflow=args.memcpy_fixture,
            memset_overflow=args.memset_fixture,
            deep_aliasing=args.deep_alias_fixture,
            const_correctness=args.const_fixture,
        )
        results = {
            "memcpy": run_analyzer(args.analyzer, fixtures.memcpy_overflow),
            "memset": run_analyzer(args.analyzer, fixtures.memset_overflow),
            "aliasing": run_analyzer(args.analyzer, fixtures.deep_aliasing),
            "const": run_analyzer(args.analyzer, fixtures.const_correctness),
        }

        reports = [
            (results["memcpy"], prove_memcpy(results["memcpy"])),
            (results["memset"], prove_memset(results["memset"])),
            (results["aliasing"], prove_deep_aliasing(results["aliasing"])),
            (results["const"], prove_const_correctness(results["const"])),
        ]
        all_checks = [check for _, checks in reports for check in checks]
        all_found = all(check.found for check in all_checks)

        print_log(f"BTP-STACK-ANALYZER-F2: {status_label(all_found)}")
        for result, checks in reports:
            print_report(result, checks)
        return 0 if all_found else 1
    except Exception as exc:
        print_log(f"BTP-STACK-ANALYZER-F2: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
