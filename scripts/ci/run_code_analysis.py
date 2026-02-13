
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Iterable

SUPPORTED_EXTENSIONS = {
    ".c",
    ".cc",
    ".cp",
    ".cpp",
    ".cxx",
    ".c++",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run stack_usage_analyzer in CI, export JSON/SARIF reports, "
            "and fail the job based on severity thresholds."
        )
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        help=(
            "Input files (.c/.cpp/.ll). If omitted and --compdb is provided, "
            "inputs are discovered from compile_commands.json."
        ),
    )
    parser.add_argument(
        "--analyzer",
        default="build/stack_usage_analyzer",
        help="Path to stack_usage_analyzer binary (default: build/stack_usage_analyzer).",
    )
    parser.add_argument(
        "--compdb",
        default="",
        help=(
            "Path to compile_commands.json (file or directory). Passed to analyzer and "
            "used as source discovery when no explicit inputs are provided."
        ),
    )
    parser.add_argument(
        "--json-out",
        default="artifacts/stack-usage.json",
        help="Output JSON report path (default: artifacts/stack-usage.json).",
    )
    parser.add_argument(
        "--sarif-out",
        default="artifacts/stack-usage.sarif",
        help=(
            "Output SARIF report path (default: artifacts/stack-usage.sarif). "
            "Use --sarif-out '' to disable SARIF emission."
        ),
    )
    parser.add_argument(
        "--fail-on",
        choices=("none", "error", "warning"),
        default="error",
        help=(
            "CI gate policy: none (never fail on findings), error (fail on errors only), "
            "warning (fail on warnings and errors)."
        ),
    )
    parser.add_argument(
        "--analyzer-arg",
        action="append",
        default=[],
        help="Extra argument forwarded to analyzer (repeatable).",
    )
    parser.add_argument(
        "--base-dir",
        default="",
        help=(
            "Base directory stripped from SARIF URIs to produce relative paths. "
            "Passed as --base-dir=<value> to the analyzer for SARIF output."
        ),
    )
    return parser.parse_args()


def normalize_path(path: str) -> str:
    return str(Path(path).expanduser())


def resolve_compdb(path_value: str) -> Path:
    candidate = Path(path_value).expanduser()
    if candidate.is_dir():
        candidate = candidate / "compile_commands.json"
    return candidate


def is_source_like(path: Path) -> bool:
    suffix = path.suffix.lower()
    return suffix in SUPPORTED_EXTENSIONS or suffix == ".ll"


def discover_inputs_from_compdb(compdb_path: Path) -> list[str]:
    if not compdb_path.is_file():
        raise FileNotFoundError(f"compile_commands.json not found: {compdb_path}")

    payload = json.loads(compdb_path.read_text(encoding="utf-8"))
    if not isinstance(payload, list):
        raise ValueError("compile_commands.json must be a JSON array")

    discovered: list[str] = []
    seen: set[str] = set()

    for entry in payload:
        if not isinstance(entry, dict):
            continue
        raw_file = entry.get("file")
        if not isinstance(raw_file, str) or not raw_file.strip():
            continue

        file_path = Path(raw_file)
        if not file_path.is_absolute():
            directory = entry.get("directory")
            if isinstance(directory, str) and directory.strip():
                file_path = Path(directory) / file_path
            else:
                file_path = compdb_path.parent / file_path

        file_path = file_path.resolve()
        if not is_source_like(file_path):
            continue

        file_as_str = str(file_path)
        if file_as_str in seen:
            continue
        seen.add(file_as_str)
        discovered.append(file_as_str)

    discovered.sort()
    return discovered


def ensure_parent(path_value: str) -> None:
    path = Path(path_value).expanduser()
    if path.parent and not path.parent.exists():
        path.parent.mkdir(parents=True, exist_ok=True)


def run_and_capture(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False)


def severity_counts(diagnostics: Iterable[dict]) -> tuple[int, int, int]:
    errors = 0
    warnings = 0
    infos = 0

    for diag in diagnostics:
        sev = str(diag.get("severity", "")).strip().lower()
        if sev == "error":
            errors += 1
        elif sev == "warning":
            warnings += 1
        elif sev == "info":
            infos += 1

    return errors, warnings, infos


def should_fail(policy: str, errors: int, warnings: int) -> bool:
    if policy == "none":
        return False
    if policy == "warning":
        return (errors + warnings) > 0
    return errors > 0


def build_analyzer_command(
    analyzer: str,
    inputs: list[str],
    analyzer_args: list[str],
    compdb: str,
    output_format: str,
) -> list[str]:
    command = [analyzer, *inputs, f"--format={output_format}"]
    if compdb:
        command.append(f"--compile-commands={compdb}")
    command.extend(analyzer_args)
    return command


def main() -> int:
    args = parse_args()

    analyzer_path = normalize_path(args.analyzer)
    if not Path(analyzer_path).is_file():
        print(f"Analyzer binary not found: {analyzer_path}", file=sys.stderr)
        return 1

    compdb = normalize_path(args.compdb) if args.compdb else ""

    inputs = [normalize_path(p) for p in args.inputs]
    if not inputs and compdb:
        try:
            inputs = discover_inputs_from_compdb(resolve_compdb(compdb))
        except Exception as exc:
            print(f"Failed to discover inputs from compile_commands.json: {exc}", file=sys.stderr)
            return 1

    if not inputs:
        print("No input files provided. Pass files explicitly or use --compdb.", file=sys.stderr)
        return 1

    ensure_parent(args.json_out)
    if args.sarif_out:
        ensure_parent(args.sarif_out)

    json_cmd = build_analyzer_command(
        analyzer=analyzer_path,
        inputs=inputs,
        analyzer_args=args.analyzer_arg,
        compdb=compdb,
        output_format="json",
    )
    print(f"Running analyzer (JSON) on {len(inputs)} file(s).")
    json_run = run_and_capture(json_cmd)
    if json_run.stderr:
        print(json_run.stderr, file=sys.stderr, end="")
    if json_run.returncode != 0:
        if json_run.stdout:
            print(json_run.stdout, file=sys.stderr, end="")
        return json_run.returncode

    Path(args.json_out).write_text(json_run.stdout, encoding="utf-8")

    try:
        json_payload = json.loads(json_run.stdout)
    except json.JSONDecodeError as exc:
        print(f"Analyzer produced invalid JSON: {exc}", file=sys.stderr)
        return 1

    diagnostics = json_payload.get("diagnostics", [])
    if not isinstance(diagnostics, list):
        print("Analyzer JSON is missing diagnostics array.", file=sys.stderr)
        return 1

    errors, warnings, infos = severity_counts(diagnostics)
    print(f"Findings summary: errors={errors}, warnings={warnings}, infos={infos}")

    if args.sarif_out:
        sarif_args = list(args.analyzer_arg)
        if args.base_dir:
            sarif_args.append(f"--base-dir={args.base_dir}")
        sarif_cmd = build_analyzer_command(
            analyzer=analyzer_path,
            inputs=inputs,
            analyzer_args=sarif_args,
            compdb=compdb,
            output_format="sarif",
        )
        print("Running analyzer (SARIF export).")
        sarif_run = run_and_capture(sarif_cmd)
        if sarif_run.stderr:
            print(sarif_run.stderr, file=sys.stderr, end="")
        if sarif_run.returncode != 0:
            if sarif_run.stdout:
                print(sarif_run.stdout, file=sys.stderr, end="")
            return sarif_run.returncode
        Path(args.sarif_out).write_text(sarif_run.stdout, encoding="utf-8")

    if should_fail(args.fail_on, errors, warnings):
        print(f"CI gate failed (fail-on={args.fail_on}).", file=sys.stderr)
        return 2

    print(f"CI gate passed (fail-on={args.fail_on}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
