#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for SARIF, JSON, and human-readable output.

F5 proves that stack_usage_analyzer can emit:
  - human-readable CLI output,
  - JSON output suitable for CI automation,
  - SARIF output suitable for GUI/security tooling.

The proof runs the three formats on three existing repository fixtures and
checks that diagnostic counts are consistent across the formats.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


DEFAULT_ANALYZER = Path(os.environ.get("CORETRACE_STACK_ANALYZER", "./build/stack_usage_analyzer"))
DEFAULT_FIXTURES = (
    Path("test/security/buffer-overflow/01_buffer_overflow.c"),
    Path("test/cpy-buffer/bad-usage-memcpy.c"),
    Path("test/local-storage/c/stack-callee-caller.c"),
)
REPORT_FORMATS = ("human", "json", "sarif")
SUMMARY_RE = re.compile(r"Diagnostics summary: info=(\d+), warning=(\d+), error=(\d+)")
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
class AnalyzerOutput:
    fixture: Path
    report_format: str
    command: tuple[str, ...]
    stdout: str
    stderr: str
    returncode: int


@dataclass(frozen=True)
class ProofCheck:
    name: str
    found: bool
    detail: str


@dataclass(frozen=True)
class FixtureProof:
    fixture: Path
    outputs: dict[str, AnalyzerOutput]
    checks: list[ProofCheck]


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Prove SARIF, JSON, and human-readable output across three "
            "stack_usage_analyzer fixtures."
        )
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help="Path to stack_usage_analyzer, or CORETRACE_STACK_ANALYZER.",
    )
    parser.add_argument(
        "--fixture",
        action="append",
        type=Path,
        default=[],
        help="Fixture to analyze. Repeat exactly three times to override defaults.",
    )
    parser.add_argument(
        "--show-analyzer-output",
        action="store_true",
        help="Compatibility option; captured stdout/stderr are printed by default.",
    )
    args, analyzer_args = parser.parse_known_args(argv)
    args.analyzer_args = normalize_extra_args(analyzer_args)
    return args


def normalize_extra_args(extra_args: Sequence[str]) -> list[str]:
    args = list(extra_args)
    if args and args[0] == "--":
        return args[1:]
    return args


def selected_fixtures(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    fixtures = tuple(args.fixture) if args.fixture else DEFAULT_FIXTURES
    if len(fixtures) != 3:
        raise ValueError(f"F5 proof expects exactly 3 fixtures, got {len(fixtures)}.")
    return fixtures  # type: ignore[return-value]


def validate_inputs(analyzer: Path, fixtures: Sequence[Path]) -> None:
    if not analyzer.exists():
        raise FileNotFoundError(f"Analyzer not found: {analyzer}")
    for fixture in fixtures:
        if not fixture.exists():
            raise FileNotFoundError(f"Fixture not found: {fixture}")


def run_analyzer(
    analyzer: Path,
    fixture: Path,
    report_format: str,
    extra_args: Sequence[str],
) -> AnalyzerOutput:
    command = (str(analyzer), str(fixture), f"--format={report_format}", *extra_args)
    result = subprocess.run(
        command,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return AnalyzerOutput(
        fixture=fixture,
        report_format=report_format,
        command=command,
        stdout=result.stdout,
        stderr=result.stderr,
        returncode=result.returncode,
    )


def parse_json_object(output: AnalyzerOutput) -> tuple[dict[str, Any] | None, str | None]:
    try:
        payload = json.loads(output.stdout)
    except json.JSONDecodeError as exc:
        return None, str(exc)
    if not isinstance(payload, dict):
        return None, f"{output.report_format} output is not a JSON object."
    return payload, None


def json_diagnostics(payload: dict[str, Any] | None) -> list[dict[str, Any]]:
    if payload is None:
        return []
    diagnostics = payload.get("diagnostics")
    if not isinstance(diagnostics, list):
        return []
    return [diag for diag in diagnostics if isinstance(diag, dict)]


def json_summary_total(payload: dict[str, Any] | None) -> int | None:
    if payload is None:
        return None
    summary = payload.get("diagnosticsSummary")
    if not isinstance(summary, dict):
        return None
    values = [summary.get("info"), summary.get("warning"), summary.get("error")]
    if not all(isinstance(value, int) for value in values):
        return None
    return sum(values)


def human_summary_total(stdout: str) -> int | None:
    match = SUMMARY_RE.search(stdout)
    if match is None:
        return None
    return sum(int(value) for value in match.groups())


def sarif_runs(payload: dict[str, Any] | None) -> list[dict[str, Any]]:
    if payload is None:
        return []
    runs = payload.get("runs")
    if not isinstance(runs, list):
        return []
    return [run for run in runs if isinstance(run, dict)]


def sarif_driver(payload: dict[str, Any] | None) -> dict[str, Any]:
    runs = sarif_runs(payload)
    if not runs:
        return {}
    tool = runs[0].get("tool")
    if not isinstance(tool, dict):
        return {}
    driver = tool.get("driver")
    return driver if isinstance(driver, dict) else {}


def sarif_results(payload: dict[str, Any] | None) -> list[dict[str, Any]]:
    runs = sarif_runs(payload)
    if not runs:
        return []
    results = runs[0].get("results")
    if not isinstance(results, list):
        return []
    return [result for result in results if isinstance(result, dict)]


def sarif_rules(payload: dict[str, Any] | None) -> list[dict[str, Any]]:
    rules = sarif_driver(payload).get("rules")
    if not isinstance(rules, list):
        return []
    return [rule for rule in rules if isinstance(rule, dict)]


def command_success_check(output: AnalyzerOutput) -> ProofCheck:
    return ProofCheck(
        name=f"{output.report_format} command completed",
        found=output.returncode == 0,
        detail=f"expected exit code 0, got {output.returncode}",
    )


def validate_human_output(output: AnalyzerOutput) -> list[ProofCheck]:
    if output.returncode != 0:
        return [command_success_check(output)]
    return [
        command_success_check(output),
        ProofCheck(
            name="human-output mode header",
            found=bool(re.search(r"^Mode: (IR|ABI)$", output.stdout, re.MULTILINE)),
            detail="expected a readable 'Mode: IR' or 'Mode: ABI' header",
        ),
        ProofCheck(
            name="human-output function blocks",
            found="Function:" in output.stdout,
            detail="expected at least one readable Function block",
        ),
        ProofCheck(
            name="human-output stack fields",
            found="local stack:" in output.stdout and "max stack (including callees):" in output.stdout,
            detail="expected local and max stack lines",
        ),
        ProofCheck(
            name="human-output diagnostics summary",
            found=human_summary_total(output.stdout) is not None,
            detail="expected Diagnostics summary: info=N, warning=N, error=N",
        ),
    ]


def validate_json_output(output: AnalyzerOutput, payload: dict[str, Any] | None, error: str | None) -> list[ProofCheck]:
    if output.returncode != 0:
        return [command_success_check(output)]
    functions = payload.get("functions") if payload else None
    diagnostics = json_diagnostics(payload)
    first_diag = diagnostics[0] if diagnostics else {}
    location = first_diag.get("location") if isinstance(first_diag, dict) else None
    details = first_diag.get("details") if isinstance(first_diag, dict) else None
    meta = payload.get("meta") if payload else None
    return [
        command_success_check(output),
        ProofCheck(
            name="JSON parseable for CI",
            found=payload is not None,
            detail=error or "expected parseable JSON object",
        ),
        ProofCheck(
            name="JSON tool metadata",
            found=isinstance(meta, dict)
            and meta.get("tool") == "ctrace-stack-analyzer"
            and isinstance(meta.get("inputFile"), str),
            detail="expected meta.tool and meta.inputFile",
        ),
        ProofCheck(
            name="JSON functions array",
            found=isinstance(functions, list) and bool(functions),
            detail="expected non-empty functions[]",
        ),
        ProofCheck(
            name="JSON diagnostics array",
            found=bool(diagnostics)
            and isinstance(first_diag.get("ruleId"), str)
            and isinstance(first_diag.get("severity"), str)
            and isinstance(location, dict)
            and isinstance(details, dict),
            detail="expected diagnostics[] with ruleId, severity, location, and details",
        ),
        ProofCheck(
            name="JSON diagnostics summary",
            found=json_summary_total(payload) is not None,
            detail="expected diagnosticsSummary with info/warning/error counters",
        ),
    ]


def validate_sarif_output(output: AnalyzerOutput, payload: dict[str, Any] | None, error: str | None) -> list[ProofCheck]:
    if output.returncode != 0:
        return [command_success_check(output)]
    driver = sarif_driver(payload)
    rules = sarif_rules(payload)
    results = sarif_results(payload)
    first_result = results[0] if results else {}
    message = first_result.get("message") if isinstance(first_result, dict) else None
    locations = first_result.get("locations") if isinstance(first_result, dict) else None
    first_location = locations[0] if isinstance(locations, list) and locations else None
    physical = first_location.get("physicalLocation") if isinstance(first_location, dict) else None
    return [
        command_success_check(output),
        ProofCheck(
            name="SARIF parseable for GUI",
            found=payload is not None,
            detail=error or "expected parseable SARIF JSON object",
        ),
        ProofCheck(
            name="SARIF 2.1.0 envelope",
            found=payload is not None
            and payload.get("version") == "2.1.0"
            and "sarif-2.1.0" in str(payload.get("$schema", "")),
            detail="expected SARIF version 2.1.0 and schema URI",
        ),
        ProofCheck(
            name="SARIF tool driver",
            found=driver.get("name") == "coretrace-stack-analyzer",
            detail="expected tool.driver.name=coretrace-stack-analyzer",
        ),
        ProofCheck(
            name="SARIF rule catalog",
            found=bool(rules) and all(isinstance(rule.get("id"), str) for rule in rules),
            detail="expected tool.driver.rules[] with rule ids",
        ),
        ProofCheck(
            name="SARIF results for GUI",
            found=bool(results)
            and isinstance(first_result.get("ruleId"), str)
            and isinstance(first_result.get("level"), str)
            and isinstance(message, dict)
            and isinstance(message.get("text"), str)
            and isinstance(physical, dict),
            detail="expected results[] with ruleId, level, message.text, and physical location",
        ),
    ]


def validate_cross_format_counts(
    human_output: AnalyzerOutput,
    json_payload: dict[str, Any] | None,
    sarif_payload: dict[str, Any] | None,
) -> list[ProofCheck]:
    human_total = human_summary_total(human_output.stdout)
    json_total = json_summary_total(json_payload)
    json_diag_count = len(json_diagnostics(json_payload)) if json_payload is not None else None
    sarif_result_count = len(sarif_results(sarif_payload)) if sarif_payload is not None else None
    counts = {
        "human": human_total,
        "jsonSummary": json_total,
        "jsonDiagnostics": json_diag_count,
        "sarifResults": sarif_result_count,
    }
    present = [value for value in counts.values() if value is not None]
    return [
        ProofCheck(
            name="cross-format diagnostic count consistency",
            found=len(present) == 4 and len(set(present)) == 1,
            detail=f"expected equal diagnostic counts across formats, got {counts}",
        )
    ]


def prove_fixture(analyzer: Path, fixture: Path, extra_args: Sequence[str]) -> FixtureProof:
    outputs = {
        report_format: run_analyzer(analyzer, fixture, report_format, extra_args)
        for report_format in REPORT_FORMATS
    }
    json_payload, json_error = parse_json_object(outputs["json"])
    sarif_payload, sarif_error = parse_json_object(outputs["sarif"])

    checks: list[ProofCheck] = []
    checks.extend(validate_human_output(outputs["human"]))
    checks.extend(validate_json_output(outputs["json"], json_payload, json_error))
    checks.extend(validate_sarif_output(outputs["sarif"], sarif_payload, sarif_error))
    checks.extend(validate_cross_format_counts(outputs["human"], json_payload, sarif_payload))
    return FixtureProof(fixture=fixture, outputs=outputs, checks=checks)


def colored(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def status_label(found: bool) -> str:
    return colored("PASS", GREEN) if found else colored("NONE", RED)


def print_fixture_report(proof: FixtureProof) -> None:
    print_log(SEPARATOR)
    print_log(colored(str(proof.fixture), PURPLE))
    for check in proof.checks:
        print_log(f"- {check.name}: {status_label(check.found)}")
        if not check.found:
            print_log(f"  detail: {check.detail}")
    for report_format in REPORT_FORMATS:
        command = proof.outputs[report_format].command
        print_log(f"{report_format}-command: {' '.join(command)}")


def print_captured_outputs(proofs: Sequence[FixtureProof]) -> None:
    for proof in proofs:
        for report_format in REPORT_FORMATS:
            output = proof.outputs[report_format]
            print_log(SEPARATOR)
            print_log(colored(str(output.fixture), PURPLE))
            print_log(f"format: {report_format.upper()}")
            if output.stdout:
                print_log("stdout:")
                print_output_block(output.stdout)
            if output.stderr:
                print_log("stderr:")
                print_output_block(output.stderr)


def print_output_block(output: str) -> None:
    for line in output.rstrip().splitlines():
        print_log(line)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    try:
        fixtures = selected_fixtures(args)
        validate_inputs(args.analyzer, fixtures)
        proofs = [prove_fixture(args.analyzer, fixture, args.analyzer_args) for fixture in fixtures]
        all_checks = [check for proof in proofs for check in proof.checks]
        all_found = all(check.found for check in all_checks)

        print_log(f"BTP-STACK-ANALYZER-F5: {status_label(all_found)}")
        print_log("feature: SARIF/JSON and human-readable output")
        print_log(f"files-tested: {len(fixtures)}")
        for proof in proofs:
            print_fixture_report(proof)
        print_log("analyzer-output: enabled")
        print_captured_outputs(proofs)
        return 0 if all_found else 1
    except Exception as exc:
        print_log(f"BTP-STACK-ANALYZER-F5: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
