#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for stack_usage_analyzer feature checks.

The analyzer's human output prints demangled names when --demangle is enabled,
while its JSON report keeps the raw IR symbol names. Running both modes with
--demangle gives a stable tool-level proof without relying on hardcoded fixture
names or external demangling tools.

The stack-buffer-overflow proof intentionally reuses the repository security
fixture instead of generating temporary C/C++ source. That keeps the proof tied
to the same static-analysis contract exercised by the normal test suite.
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
from typing import Sequence


DEFAULT_ANALYZER = Path(os.environ.get("CORETRACE_STACK_ANALYZER", "./build/stack_usage_analyzer"))
DEFAULT_STACK_BUFFER_FIXTURE = Path("test/security/buffer-overflow/01_buffer_overflow.c")
SUPPORTED_SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".c++",
}
FUNCTION_LINE_RE = re.compile(r"^Function:\s*(?P<name>.+?)\s*$")


@dataclass(frozen=True)
class AnalyzerCommand:
    argv: tuple[str, ...]


@dataclass(frozen=True)
class FunctionNamePair:
    mangled: str
    demangled: str


@dataclass(frozen=True)
class StackBufferOverflowProof:
    fixture: Path
    function: str
    variable: str
    rule_id: str
    severity: str
    message: str


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run stack_usage_analyzer proof checks for demangling and static "
            "stack-buffer overflow diagnostics."
        ),
        epilog="Extra stack_usage_analyzer arguments can be appended after '--'.",
    )
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        help="C or C++ source file used by the demangle proof.",
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help=(
            "Path to stack_usage_analyzer. Defaults to CORETRACE_STACK_ANALYZER "
            "or ./build/stack_usage_analyzer."
        ),
    )
    parser.add_argument(
        "--proof",
        choices=("demangle", "stack-buffer-overflow", "all"),
        default="demangle",
        help=(
            "Proof to run. 'demangle' keeps the original behavior, "
            "'stack-buffer-overflow' uses an existing repository fixture, "
            "and 'all' runs both."
        ),
    )
    parser.add_argument(
        "--stack-buffer-fixture",
        type=Path,
        default=DEFAULT_STACK_BUFFER_FIXTURE,
        help=(
            "Existing C/C++ fixture used by --proof stack-buffer-overflow. "
            "Defaults to test/security/buffer-overflow/01_buffer_overflow.c."
        ),
    )
    parser.add_argument(
        "--only-function",
        action="append",
        default=[],
        help=(
            "Forwarded to the analyzer as --only-function=<name> for the "
            "demangle proof. Can be repeated."
        ),
    )
    parser.add_argument(
        "--show-analyzer-output",
        action="store_true",
        help="Print captured analyzer stdout/stderr when the proof succeeds.",
    )
    args, analyzer_args = parser.parse_known_args(argv)
    args.analyzer_args = analyzer_args
    return args


def normalize_extra_args(extra_args: Sequence[str]) -> list[str]:
    args = list(extra_args)
    if args and args[0] == "--":
        return args[1:]
    return args


def validate_analyzer(analyzer: Path) -> None:
    if not analyzer.exists():
        raise FileNotFoundError(f"Analyzer not found: {analyzer}")


def validate_source_file(source: Path) -> None:
    if not source.exists():
        raise FileNotFoundError(f"Input source not found: {source}")
    if source.suffix.lower() not in SUPPORTED_SOURCE_SUFFIXES:
        supported = ", ".join(sorted(SUPPORTED_SOURCE_SUFFIXES))
        raise ValueError(f"Expected a C/C++ source file ({supported}), got: {source}")


def build_analyzer_command(
    analyzer: Path,
    source: Path,
    only_functions: Sequence[str],
    extra_args: Sequence[str],
    *,
    output_json: bool,
) -> AnalyzerCommand:
    argv = [
        str(analyzer),
        str(source),
        "--demangle",
        "--print-effective-config",
    ]
    if output_json:
        argv.append("--format=json")
    for function_name in only_functions:
        argv.append(f"--only-function={function_name}")
    argv.extend(extra_args)
    return AnalyzerCommand(tuple(argv))


def build_stack_buffer_command(
    analyzer: Path,
    fixture: Path,
    extra_args: Sequence[str],
) -> AnalyzerCommand:
    argv = [
        str(analyzer),
        str(fixture),
        "--format=json",
    ]
    argv.extend(extra_args)
    return AnalyzerCommand(tuple(argv))


def run_analyzer(command: AnalyzerCommand) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command.argv,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


def require_success(result: subprocess.CompletedProcess[str], command: AnalyzerCommand) -> None:
    if result.returncode == 0:
        return
    details = [
        f"Analyzer command failed with exit code {result.returncode}:",
        " ".join(command.argv),
    ]
    if result.stdout:
        details.extend(["--- stdout ---", result.stdout.rstrip()])
    if result.stderr:
        details.extend(["--- stderr ---", result.stderr.rstrip()])
    raise RuntimeError("\n".join(details))


def require_demangle_enabled(result: subprocess.CompletedProcess[str]) -> None:
    effective_config = f"{result.stderr}\n{result.stdout}"
    if "demangle: true" not in effective_config:
        raise RuntimeError("The analyzer did not report 'demangle: true' in effective config.")


def parse_demangled_human_functions(stdout: str) -> list[str]:
    names: list[str] = []
    for line in stdout.splitlines():
        match = FUNCTION_LINE_RE.match(line)
        if match:
            names.append(match.group("name").strip())
    return names


def parse_mangled_json_functions(stdout: str) -> list[str]:
    payload = parse_json_report(stdout)
    functions = payload.get("functions")
    if not isinstance(functions, list):
        raise ValueError("Analyzer JSON output does not contain a functions array.")

    names: list[str] = []
    for function in functions:
        if not isinstance(function, dict):
            continue
        name = function.get("name")
        if isinstance(name, str):
            names.append(name)
    return names


def parse_json_report(stdout: str) -> dict:
    payload = json.loads(stdout)
    if not isinstance(payload, dict):
        raise ValueError("Analyzer JSON output is not a JSON object.")
    return payload


def strip_llvm_symbol_prefix(symbol: str) -> str:
    return symbol[1:] if symbol.startswith("\x01") else symbol


def is_itanium_mangled(symbol: str) -> bool:
    normalized = strip_llvm_symbol_prefix(symbol)
    return normalized.startswith("_Z") or normalized.startswith("__Z")


def collect_itanium_pairs(mangled_names: Sequence[str], demangled_names: Sequence[str]) -> list[FunctionNamePair]:
    if len(mangled_names) != len(demangled_names):
        raise RuntimeError(
            "Human and JSON analyzer outputs returned different function counts "
            f"({len(demangled_names)} human vs {len(mangled_names)} JSON)."
        )

    pairs: list[FunctionNamePair] = []
    for mangled, demangled in zip(mangled_names, demangled_names):
        if not is_itanium_mangled(mangled):
            continue
        if is_itanium_mangled(demangled) or demangled == mangled:
            raise RuntimeError(
                "Demangle mode did not produce a stable demangled name for "
                f"{mangled!r}; human output was {demangled!r}."
            )
        pairs.append(FunctionNamePair(mangled=mangled, demangled=demangled))
    return pairs


def collect_stack_buffer_overflow_proofs(payload: dict, fixture: Path) -> list[StackBufferOverflowProof]:
    diagnostics = payload.get("diagnostics")
    if not isinstance(diagnostics, list):
        raise ValueError("Analyzer JSON output does not contain a diagnostics array.")

    proofs: list[StackBufferOverflowProof] = []
    for diagnostic in diagnostics:
        if not isinstance(diagnostic, dict):
            continue
        if diagnostic.get("severity") != "WARNING":
            continue
        if diagnostic.get("ruleId") != "StackBufferOverflow":
            continue

        location = diagnostic.get("location")
        details = diagnostic.get("details")
        if not isinstance(location, dict) or not isinstance(details, dict):
            continue

        message = details.get("message")
        variable_aliasing = details.get("variableAliasing")
        function = location.get("function")
        if not isinstance(message, str) or not isinstance(function, str):
            continue
        if "stack buffer overflow on variable" not in message:
            continue
        if "buf" not in message:
            continue
        if isinstance(variable_aliasing, list) and "buf" not in variable_aliasing:
            continue

        proofs.append(
            StackBufferOverflowProof(
                fixture=fixture,
                function=function,
                variable="buf",
                rule_id="StackBufferOverflow",
                severity="WARNING",
                message=" ".join(message.split()),
            )
        )
    return proofs


def print_demangle_result(
    source: Path,
    human_command: AnalyzerCommand,
    json_command: AnalyzerCommand,
    pairs: Sequence[FunctionNamePair],
) -> None:
    print("Itanium ABI demangle proof: PASS")
    print(f"input: {source}")
    print("demangle-mode: enabled")
    print(f"symbols-found: {len(pairs)}")
    for pair in pairs:
        print(f"mangled-symbol: {pair.mangled}")
        print(f"demangled-symbol: {pair.demangled}")
    print(f"human-command: {' '.join(human_command.argv)}")
    print(f"json-command: {' '.join(json_command.argv)}")


def print_stack_buffer_overflow_result(
    command: AnalyzerCommand,
    proofs: Sequence[StackBufferOverflowProof],
) -> None:
    print("Static stack-buffer overflow proof: PASS")
    for proof in proofs:
        print(f"fixture: {proof.fixture}")
        print(f"rule-id: {proof.rule_id}")
        print(f"severity: {proof.severity}")
        print(f"function: {proof.function}")
        print(f"stack-buffer: {proof.variable}")
        print(f"diagnostic: {proof.message}")
    print(f"json-command: {' '.join(command.argv)}")


def run_demangle_proof(
    analyzer: Path,
    source: Path,
    only_functions: Sequence[str],
    extra_args: Sequence[str],
    *,
    show_analyzer_output: bool,
) -> None:
    validate_source_file(source)

    human_command = build_analyzer_command(
        analyzer,
        source,
        only_functions,
        extra_args,
        output_json=False,
    )
    json_command = build_analyzer_command(
        analyzer,
        source,
        only_functions,
        extra_args,
        output_json=True,
    )

    human_result = run_analyzer(human_command)
    require_success(human_result, human_command)
    require_demangle_enabled(human_result)

    json_result = run_analyzer(json_command)
    require_success(json_result, json_command)
    require_demangle_enabled(json_result)

    demangled_names = parse_demangled_human_functions(human_result.stdout)
    mangled_names = parse_mangled_json_functions(json_result.stdout)
    pairs = collect_itanium_pairs(mangled_names, demangled_names)
    if not pairs:
        raise RuntimeError(
            "No Itanium ABI mangled function symbol was found. A plain C file often emits "
            "unmangled C symbols; use a C++ input or a C source with explicit Itanium "
            "ABI asm labels to exercise this proof."
        )

    print_demangle_result(source, human_command, json_command, pairs)

    if show_analyzer_output:
        print("--- analyzer human stdout ---")
        print(human_result.stdout.rstrip())
        if human_result.stderr:
            print("--- analyzer human stderr ---")
            print(human_result.stderr.rstrip())
        print("--- analyzer json stdout ---")
        print(json_result.stdout.rstrip())
        if json_result.stderr:
            print("--- analyzer json stderr ---")
            print(json_result.stderr.rstrip())


def run_stack_buffer_overflow_proof(
    analyzer: Path,
    fixture: Path,
    extra_args: Sequence[str],
    *,
    show_analyzer_output: bool,
) -> None:
    validate_source_file(fixture)

    command = build_stack_buffer_command(analyzer, fixture, extra_args)
    result = run_analyzer(command)
    require_success(result, command)

    payload = parse_json_report(result.stdout)
    proofs = collect_stack_buffer_overflow_proofs(payload, fixture)
    if not proofs:
        raise RuntimeError(
            "No StackBufferOverflow warning was found for stack-allocated buffer 'buf' "
            f"in fixture {fixture}."
        )

    print_stack_buffer_overflow_result(command, proofs)

    if show_analyzer_output:
        print("--- analyzer json stdout ---")
        print(result.stdout.rstrip())
        if result.stderr:
            print("--- analyzer json stderr ---")
            print(result.stderr.rstrip())


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    source = args.source
    analyzer = args.analyzer
    extra_args = normalize_extra_args(args.analyzer_args)

    try:
        validate_analyzer(analyzer)

        if args.proof in ("demangle", "all"):
            if source is None:
                raise ValueError("The demangle proof requires a C/C++ source argument.")
            run_demangle_proof(
                analyzer,
                source,
                args.only_function,
                extra_args,
                show_analyzer_output=args.show_analyzer_output,
            )

        if args.proof in ("stack-buffer-overflow", "all"):
            run_stack_buffer_overflow_proof(
                analyzer,
                args.stack_buffer_fixture,
                extra_args,
                show_analyzer_output=args.show_analyzer_output,
            )

        return 0
    except Exception as exc:
        print(f"stack_usage_analyzer proof: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
