#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Proof script for Itanium ABI mangle/demangle support.

The proof runs stack_usage_analyzer twice over a C/C++ source file:
  - JSON output keeps the raw LLVM/Itanium symbols, e.g. _Z...
  - human output with --demangle prints readable C++ signatures.

The two views are paired to prove that the tool exposes mangled symbols,
demangles them with parameters, and returns stable results across repeated
runs.
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
DEFAULT_SOURCE = Path("test/test.cpp")
SUPPORTED_SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".c++",
}
FUNCTION_LINE_RE = re.compile(r"^Function:\s*(?P<name>.+?)\s*$")
GREEN = "\033[32m"
RED = "\033[31m"
PURPLE = "\033[35m"
RESET = "\033[0m"
SEPARATOR = "--------"
EXPECTED_DEFAULT_PAIRS = {
    "_ZN12demo_symbols17scalar_parametersEidc": "demo_symbols::scalar_parameters(int, double, char)",
    "_ZN12demo_symbols18pointer_parametersEPKiPKcb": "demo_symbols::pointer_parameters(int const*, char const*, bool)",
    "_ZN12demo_symbols20reference_parametersERKNS_6SampleEfm": (
        "demo_symbols::reference_parameters(demo_symbols::Sample const&, float, unsigned long)"
    ),
    "_ZN12demo_symbols10overloadedEi": "demo_symbols::overloaded(int)",
    "_ZN12demo_symbols10overloadedEii": "demo_symbols::overloaded(int, int)",
}


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
class AnalyzerCommand:
    argv: tuple[str, ...]


@dataclass(frozen=True)
class AnalyzerOutput:
    command: AnalyzerCommand
    stdout: str
    stderr: str
    returncode: int


@dataclass(frozen=True)
class FunctionNamePair:
    mangled: str
    demangled: str


@dataclass(frozen=True)
class ProofRun:
    source: Path
    human: AnalyzerOutput
    json_report: AnalyzerOutput
    mangled_count: int
    demangled_count: int
    pairs: tuple[FunctionNamePair, ...]


@dataclass(frozen=True)
class ProofCheck:
    name: str
    found: bool
    detail: str


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prove Itanium ABI mangle/demangle support through stack_usage_analyzer."
    )
    parser.add_argument(
        "source",
        nargs="?",
        type=Path,
        default=DEFAULT_SOURCE,
        help="C or C++ source file used by the proof. Defaults to test/test.cpp.",
    )
    parser.add_argument(
        "--analyzer",
        type=Path,
        default=DEFAULT_ANALYZER,
        help="Path to stack_usage_analyzer, or CORETRACE_STACK_ANALYZER.",
    )
    parser.add_argument(
        "--only-function",
        action="append",
        default=[],
        help="Forwarded to the analyzer as --only-function=<name>. Can be repeated.",
    )
    parser.add_argument(
        "--minimum-symbols",
        type=int,
        default=1,
        help="Minimum number of Itanium symbols required for a PASS.",
    )
    parser.add_argument(
        "--show-analyzer-output",
        action="store_true",
        help="Print captured analyzer stdout/stderr when the proof completes.",
    )
    args, analyzer_args = parser.parse_known_args(argv)
    args.analyzer_args = normalize_extra_args(analyzer_args)
    return args


def normalize_extra_args(extra_args: Sequence[str]) -> list[str]:
    args = list(extra_args)
    if args and args[0] == "--":
        return args[1:]
    return args


def validate_inputs(analyzer: Path, source: Path) -> None:
    if not analyzer.exists():
        raise FileNotFoundError(f"Analyzer not found: {analyzer}")
    if not source.exists():
        raise FileNotFoundError(f"Input source not found: {source}")
    if source.suffix.lower() not in SUPPORTED_SOURCE_SUFFIXES:
        supported = ", ".join(sorted(SUPPORTED_SOURCE_SUFFIXES))
        raise ValueError(f"Expected a C/C++ source file ({supported}), got: {source}")


def build_human_command(
    analyzer: Path,
    source: Path,
    only_functions: Sequence[str],
    extra_args: Sequence[str],
) -> AnalyzerCommand:
    argv = [str(analyzer), str(source), "--demangle", "--print-effective-config"]
    for function_name in only_functions:
        argv.append(f"--only-function={function_name}")
    argv.extend(extra_args)
    return AnalyzerCommand(tuple(argv))


def build_json_command(
    analyzer: Path,
    source: Path,
    only_functions: Sequence[str],
    extra_args: Sequence[str],
) -> AnalyzerCommand:
    argv = [str(analyzer), str(source), "--demangle", "--format=json"]
    for function_name in only_functions:
        argv.append(f"--only-function={function_name}")
    argv.extend(extra_args)
    return AnalyzerCommand(tuple(argv))


def run_analyzer(command: AnalyzerCommand) -> AnalyzerOutput:
    result = subprocess.run(
        command.argv,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return AnalyzerOutput(
        command=command,
        stdout=result.stdout,
        stderr=result.stderr,
        returncode=result.returncode,
    )


def require_success(output: AnalyzerOutput) -> None:
    if output.returncode == 0:
        return
    message = [
        f"Analyzer failed with exit code {output.returncode}",
        f"command: {' '.join(output.command.argv)}",
    ]
    if output.stdout:
        message.extend(["stdout:", output.stdout.rstrip()])
    if output.stderr:
        message.extend(["stderr:", output.stderr.rstrip()])
    raise RuntimeError("\n".join(message))


def demangle_enabled(output: AnalyzerOutput) -> bool:
    effective_config = f"{output.stdout}\n{output.stderr}"
    return "demangle: true" in effective_config


def parse_demangled_human_functions(stdout: str) -> list[str]:
    names: list[str] = []
    for line in stdout.splitlines():
        match = FUNCTION_LINE_RE.match(line)
        if match:
            names.append(match.group("name").strip())
    return names


def parse_mangled_json_functions(stdout: str) -> list[str]:
    payload = json.loads(stdout)
    if not isinstance(payload, dict):
        raise ValueError("Analyzer JSON output is not a JSON object.")
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


def strip_llvm_symbol_prefix(symbol: str) -> str:
    return symbol[1:] if symbol.startswith("\x01") else symbol


def is_itanium_mangled(symbol: str) -> bool:
    normalized = strip_llvm_symbol_prefix(symbol)
    return normalized.startswith("_Z") or normalized.startswith("__Z")


def collect_itanium_pairs(
    mangled_names: Sequence[str],
    demangled_names: Sequence[str],
) -> tuple[FunctionNamePair, ...]:
    if len(mangled_names) != len(demangled_names):
        raise RuntimeError(
            "Human and JSON outputs returned different function counts "
            f"({len(demangled_names)} human vs {len(mangled_names)} JSON)."
        )

    pairs: list[FunctionNamePair] = []
    for mangled, demangled in zip(mangled_names, demangled_names):
        if not is_itanium_mangled(mangled):
            continue
        pairs.append(FunctionNamePair(mangled=mangled, demangled=demangled))
    return tuple(pairs)


def has_parameterized_signature(pair: FunctionNamePair) -> bool:
    open_paren = pair.demangled.find("(")
    close_paren = pair.demangled.rfind(")")
    if open_paren < 0 or close_paren <= open_paren:
        return False
    parameter_list = pair.demangled[open_paren + 1 : close_paren].strip()
    return bool(parameter_list) and parameter_list != "void"


def is_default_source(source: Path) -> bool:
    try:
        return source.resolve() == DEFAULT_SOURCE.resolve()
    except FileNotFoundError:
        return source == DEFAULT_SOURCE


def expected_default_pairs_check(source: Path, only_functions: Sequence[str], pairs: Sequence[FunctionNamePair]) -> ProofCheck | None:
    if only_functions or not is_default_source(source):
        return None

    by_mangled = {pair.mangled: pair.demangled for pair in pairs}
    missing = [
        f"{mangled} -> {demangled}"
        for mangled, demangled in EXPECTED_DEFAULT_PAIRS.items()
        if by_mangled.get(mangled) != demangled
    ]
    return ProofCheck(
        name="known Itanium ABI fixture signatures",
        found=not missing,
        detail="missing or mismatched expected fixture pairs: " + "; ".join(missing),
    )


def run_proof_once(
    analyzer: Path,
    source: Path,
    only_functions: Sequence[str],
    extra_args: Sequence[str],
) -> ProofRun:
    human_command = build_human_command(analyzer, source, only_functions, extra_args)
    json_command = build_json_command(analyzer, source, only_functions, extra_args)

    human_output = run_analyzer(human_command)
    require_success(human_output)
    json_output = run_analyzer(json_command)
    require_success(json_output)

    demangled_names = parse_demangled_human_functions(human_output.stdout)
    mangled_names = parse_mangled_json_functions(json_output.stdout)
    pairs = collect_itanium_pairs(mangled_names, demangled_names)
    return ProofRun(
        source=source,
        human=human_output,
        json_report=json_output,
        mangled_count=len(mangled_names),
        demangled_count=len(demangled_names),
        pairs=pairs,
    )


def build_checks(
    first: ProofRun,
    second: ProofRun,
    *,
    only_functions: Sequence[str],
    minimum_symbols: int,
) -> list[ProofCheck]:
    pairs = first.pairs
    checks = [
        ProofCheck(
            name="demangle mode enabled",
            found=demangle_enabled(first.human),
            detail="expected --print-effective-config to contain 'demangle: true'",
        ),
        ProofCheck(
            name="Itanium mangled symbols found",
            found=len(pairs) >= minimum_symbols,
            detail=f"expected at least {minimum_symbols} Itanium _Z symbol(s), got {len(pairs)}",
        ),
        ProofCheck(
            name="demangled symbols are readable",
            found=bool(pairs)
            and all(pair.demangled != pair.mangled and not is_itanium_mangled(pair.demangled) for pair in pairs),
            detail="expected every Itanium symbol to map to a readable non-_Z function signature",
        ),
        ProofCheck(
            name="parameterized signatures visible",
            found=any(has_parameterized_signature(pair) for pair in pairs),
            detail="expected at least one demangled function signature with parameters",
        ),
        ProofCheck(
            name="stable repeated output",
            found=first.pairs == second.pairs,
            detail="expected identical mangled/demangled pairs across two analyzer runs",
        ),
    ]

    default_check = expected_default_pairs_check(first.source, only_functions, pairs)
    if default_check is not None:
        checks.append(default_check)
    return checks


def colored(text: str, color: str) -> str:
    return f"{color}{text}{RESET}"


def status_label(found: bool) -> str:
    return colored("PASS", GREEN) if found else colored("NONE", RED)


def print_report(run: ProofRun, checks: Sequence[ProofCheck]) -> None:
    all_found = all(check.found for check in checks)
    print_log(f"BTP-STACK-ANALYZER-F8: {status_label(all_found)}")
    print_log(SEPARATOR)
    print_log(colored(str(run.source), PURPLE))
    print_log("feature: Itanium ABI mangle/demangle")
    print_log(f"demangle-mode: {'enabled' if demangle_enabled(run.human) else 'disabled'}")
    print_log(f"symbols-found: {len(run.pairs)}")
    for check in checks:
        print_log(f"- {check.name}: {status_label(check.found)}")
        if not check.found:
            print_log(f"  detail: {check.detail}")
    for pair in run.pairs:
        print_log(f"mangled-symbol: {pair.mangled}")
        print_log(f"demangled-symbol: {pair.demangled}")
    print_log(f"human-command: {' '.join(run.human.command.argv)}")
    print_log(f"json-command: {' '.join(run.json_report.command.argv)}")


def print_captured_output(run: ProofRun) -> None:
    print_log(SEPARATOR)
    print_log("analyzer human stdout:")
    print_log(run.human.stdout.rstrip())
    if run.human.stderr:
        print_log("analyzer human stderr:")
        print_log(run.human.stderr.rstrip())
    print_log("analyzer json stdout:")
    print_log(run.json_report.stdout.rstrip())
    if run.json_report.stderr:
        print_log("analyzer json stderr:")
        print_log(run.json_report.stderr.rstrip())


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    try:
        validate_inputs(args.analyzer, args.source)
        first = run_proof_once(args.analyzer, args.source, args.only_function, args.analyzer_args)
        second = run_proof_once(args.analyzer, args.source, args.only_function, args.analyzer_args)
        checks = build_checks(
            first,
            second,
            only_functions=args.only_function,
            minimum_symbols=args.minimum_symbols,
        )
        print_report(first, checks)
        if args.show_analyzer_output:
            print_captured_output(first)
        return 0 if all(check.found for check in checks) else 1
    except Exception as exc:
        print_log(f"BTP-STACK-ANALYZER-F8: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
