#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for static stack-allocated buffer overflow detection.

This script validates that stack_usage_analyzer emits a structured
StackBufferOverflow diagnostic for an existing stack buffer fixture.
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
DEFAULT_FIXTURE = Path("test/security/buffer-overflow/01_buffer_overflow.c")
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
class AnalyzerResult:
    fixture: Path
    command: tuple[str, ...]
    payload: dict[str, Any]


@dataclass(frozen=True)
class StackBufferProof:
    function: str
    variable: str
    rule_id: str
    severity: str
    message: str


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prove static overflow detection on stack-allocated buffers using "
            "an existing stack_usage_analyzer fixture."
        )
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help="Path to stack_usage_analyzer, or CORETRACE_STACK_ANALYZER.",
    )
    parser.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
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


def aliases_of(diag: dict[str, Any]) -> list[str]:
    details = diag.get("details")
    if not isinstance(details, dict):
        return []
    aliases = details.get("variableAliasing")
    return [item for item in aliases if isinstance(item, str)] if isinstance(aliases, list) else []


def find_stack_buffer_overflow(payload: dict[str, Any]) -> StackBufferProof | None:
    for diag in diagnostics(payload):
        if diag.get("ruleId") != "StackBufferOverflow":
            continue
        if diag.get("severity") != "WARNING":
            continue

        location = diag.get("location")
        if not isinstance(location, dict) or location.get("function") != "vuln_off_by_one":
            continue

        message = message_of(diag)
        aliases = aliases_of(diag)
        required_fragments = (
            "potential stack buffer overflow on variable 'buf'",
            "size 10",
            "alias path: buf",
            "index variable may go up to 10",
            "array last valid index: 9",
            "write access",
        )
        if not all(fragment in message for fragment in required_fragments):
            continue
        if "buf" not in aliases:
            continue

        return StackBufferProof(
            function="vuln_off_by_one",
            variable="buf",
            rule_id="StackBufferOverflow",
            severity="WARNING",
            message=" ".join(message.split()),
        )
    return None


def colored(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def status_label(found: bool) -> str:
    return colored("PASS", GREEN) if found else colored("NONE", RED)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        result = run_analyzer(args.analyzer, args.fixture)
        proof = find_stack_buffer_overflow(result.payload)
        found = proof is not None

        print_log(f"BTP-STACK-ANALYZER-F10: {status_label(found)}")
        print_log(SEPARATOR)
        print_log(colored(str(result.fixture), PURPLE))
        print_log(f"- static stack-buffer overflow: {status_label(found)}")
        if proof is None:
            print_log("  detail: expected StackBufferOverflow warning for vuln_off_by_one stack buffer 'buf'")
        else:
            print_log(f"  rule-id: {proof.rule_id}")
            print_log(f"  severity: {proof.severity}")
            print_log(f"  function: {proof.function}")
            print_log(f"  stack-buffer: {proof.variable}")
            print_log(f"  diagnostic: {proof.message}")
        print_log(f"command: {' '.join(result.command)}")
        return 0 if found else 1
    except Exception as exc:
        print_log(f"BTP-STACK-ANALYZER-F10: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
