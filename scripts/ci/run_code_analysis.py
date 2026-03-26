#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
from pathlib import Path
from typing import Iterable

SOURCE_PATTERNS = ("*.c", "*.cc", "*.cpp", "*.cxx", "*.ll")
SUPPORTED_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".ll"}
COMPDB_COMPAT_ALLOWED_ROOTS = (Path("/tmp"), Path("/var/tmp"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run stack_usage_analyzer in CI, export JSON/SARIF reports, "
            "and apply a CI gate."
        )
    )
    parser.add_argument("inputs", nargs="*", help="Input files to analyze.")
    parser.add_argument("--analyzer", default="build/stack_usage_analyzer")
    parser.add_argument("--compdb", help="compile_commands.json (file or directory).")
    parser.add_argument("--json-out", help="Write analyzer JSON output to this file.")
    parser.add_argument("--sarif-out", help="Write analyzer SARIF output to this file.")
    parser.add_argument("--fail-on", choices=("none", "error", "warning"), default="error")
    parser.add_argument("--analyzer-arg", action="append", default=[], help="Repeatable.")
    parser.add_argument("--base-dir", help="Passed through to analyzer.")

    parser.add_argument("--inputs-from-git", action="store_true")
    parser.add_argument("--repo-root", default=".")
    parser.add_argument("--exclude", action="append", default=[], help="Repeatable glob/substr.")
    parser.add_argument(
        "--print-diagnostics",
        choices=("none", "error", "warning", "info", "all"),
        default="none",
    )
    return parser.parse_args()


def norm(path: Path) -> Path:
    try:
        return path.resolve()
    except OSError:
        return path.absolute()


def ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def dedupe(paths: Iterable[Path]) -> list[Path]:
    seen: set[str] = set()
    unique_paths: list[Path] = []
    for path in paths:
        key = str(path)
        if key in seen:
            continue
        seen.add(key)
        unique_paths.append(path)
    return unique_paths


def is_supported_source(path: Path) -> bool:
    return path.suffix.lower() in SUPPORTED_SUFFIXES


def is_within_root(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def load_compdb(compdb_arg: str) -> tuple[Path, list[dict]]:
    compdb_path = Path(compdb_arg)
    if compdb_path.is_dir():
        compdb_path = compdb_path / "compile_commands.json"
    if not compdb_path.exists():
        raise FileNotFoundError(f"compile_commands.json not found: {compdb_path}")
    compdb_entries = json.loads(compdb_path.read_text(encoding="utf-8"))
    if not isinstance(compdb_entries, list):
        raise ValueError("compile_commands.json must be a JSON array")
    return norm(compdb_path), compdb_entries


def compdb_files(compdb_path: Path, entries: list[dict]) -> list[Path]:
    selected_files: list[Path] = []
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        entry_file = entry.get("file")
        if not isinstance(entry_file, str) or not entry_file:
            continue
        resolved_file = Path(entry_file)
        if not resolved_file.is_absolute():
            directory = entry.get("directory")
            if isinstance(directory, str) and directory:
                resolved_file = Path(directory) / resolved_file
            else:
                resolved_file = compdb_path.parent / resolved_file
        # Keep original compdb identity for analyzer lookup.
        selected_files.append(norm(resolved_file))
    return dedupe(selected_files)


def ensure_compdb_compat_symlink(
    compdb_path: Path, entries: list[dict], repo_root: Path
) -> None:
    """
    If compile_commands.json was generated in another checkout root
    (e.g. /tmp/evan) and the repository is mounted elsewhere (e.g. /workspace),
    create a safe compatibility symlink so analyzer lookups still match compdb paths.
    """
    entry_dir: Path | None = None
    for entry in entries:
        if not isinstance(entry, dict):
            continue
        directory = entry.get("directory")
        if isinstance(directory, str) and directory.startswith("/"):
            entry_dir = Path(directory)
            break

    if entry_dir is None or entry_dir.exists():
        return

    try:
        compdb_build_dir = compdb_path.parent.resolve()
    except OSError:
        return

    # Only support root relocation, keep build directory name stable.
    if entry_dir.name != compdb_build_dir.name:
        return

    legacy_root = entry_dir.parent
    if str(legacy_root) in {"/", ""}:
        return

    try:
        legacy_root_resolved = legacy_root.resolve(strict=False)
    except OSError:
        legacy_root_resolved = legacy_root

    if legacy_root_resolved in COMPDB_COMPAT_ALLOWED_ROOTS:
        return
    if not any(is_within_root(legacy_root_resolved, root) for root in COMPDB_COMPAT_ALLOWED_ROOTS):
        return

    if legacy_root.exists() or legacy_root.is_symlink():
        return

    try:
        if legacy_root.resolve() == repo_root.resolve():
            return
    except OSError:
        pass

    try:
        legacy_root.parent.mkdir(parents=True, exist_ok=True)
        legacy_root.symlink_to(repo_root)
        print(f"[ci] Created compatibility symlink: {legacy_root} -> {repo_root}")
    except OSError:
        print(
            "[ci] Unable to create compatibility symlink for compile_commands paths.",
            file=sys.stderr,
        )


def discover_inputs_from_git(repo_root: Path) -> list[Path]:
    cmd = ["git", "-C", str(repo_root), "ls-files", "--", *SOURCE_PATTERNS]
    try:
        git_result = subprocess.run(cmd, check=False, capture_output=True, text=True)
    except FileNotFoundError as exc:
        raise RuntimeError(
            "git executable not found. Install git in the runtime image or avoid "
            "using --inputs-from-git."
        ) from exc

    if git_result.returncode != 0:
        msg = (git_result.stderr or git_result.stdout or f"git exited with {git_result.returncode}").strip()
        raise RuntimeError(msg)

    git_files: list[Path] = []
    for line in git_result.stdout.splitlines():
        s = line.strip()
        if s:
            git_files.append(norm(repo_root / s))
    return dedupe(git_files)


def is_excluded(path: Path, repo_root: Path, patterns: list[str]) -> bool:
    if not patterns:
        return False
    abs_s = path.as_posix()
    try:
        rel_s = path.relative_to(repo_root).as_posix()
    except ValueError:
        rel_s = abs_s

    for pat in patterns:
        if not pat:
            continue
        normalized_pattern = pat.replace("\\", "/")
        if fnmatch.fnmatch(rel_s, normalized_pattern) or fnmatch.fnmatch(abs_s, normalized_pattern):
            return True
        if normalized_pattern in rel_s or normalized_pattern in abs_s:
            return True
    return False


def sev(diag: dict) -> str:
    raw_severity = diag.get("severity")
    if raw_severity is None:
        raw_severity = diag.get("level")
    if raw_severity is None and isinstance(diag.get("details"), dict):
        raw_severity = diag["details"].get("severity")

    if isinstance(raw_severity, int):
        return {0: "INFO", 1: "WARNING", 2: "ERROR"}.get(raw_severity, "INFO")
    severity_name = str(raw_severity or "").upper()
    if severity_name in {"2", "ERROR"}:
        return "ERROR"
    if severity_name in {"1", "WARNING", "WARN"}:
        return "WARNING"
    return "INFO"


def diag_message(diag: dict) -> str:
    d = diag.get("details")
    if isinstance(d, dict) and isinstance(d.get("message"), str):
        return d["message"]
    m = diag.get("message")
    return m if isinstance(m, str) else ""


def diag_rule(diag: dict) -> str:
    d = diag.get("details")
    if isinstance(d, dict):
        for k in ("ruleId", "code"):
            if isinstance(d.get(k), str) and d[k]:
                return d[k]
    for k in ("ruleId", "code"):
        if isinstance(diag.get(k), str) and diag[k]:
            return diag[k]
    return ""


def diag_loc(diag: dict) -> tuple[str, int, int]:
    loc = diag.get("location")
    if not isinstance(loc, dict):
        return "<unknown>", 0, 0
    f = loc.get("file")
    if not isinstance(f, str) or not f:
        f = "<unknown>"
    return f, int(loc.get("startLine") or 0), int(loc.get("startColumn") or 0)


def should_print(mode: str, severity: str) -> bool:
    if mode == "none":
        return False
    if mode in {"all", "info"}:
        return True
    if mode == "warning":
        return severity in {"WARNING", "ERROR"}
    if mode == "error":
        return severity == "ERROR"
    return False


def print_diags(diags: list[dict], mode: str) -> None:
    if mode == "none":
        return
    for diag in diags:
        severity = sev(diag)
        if not should_print(mode, severity):
            continue
        file_, line, col = diag_loc(diag)
        rule_id = diag_rule(diag)
        message_lines = diag_message(diag).strip().splitlines()
        header = f"[{severity}] {file_}:{line}:{col}"
        if rule_id:
            header += f" ({rule_id})"
        if message_lines:
            header += f" {message_lines[0]}"
        print(header)
        for extra in message_lines[1:]:
            if extra.strip():
                print(f"                 {extra}")


def analyzer_cmd(
    analyzer: Path,
    inputs: list[Path],
    fmt: str,
    compdb_path: Path | None,
    base_dir: str | None,
    extra_args: list[str],
    sarif_out: str | None = None,
) -> list[str]:
    cmd = [str(analyzer), *[str(x) for x in inputs], f"--format={fmt}"]
    if compdb_path:
        cmd.append(f"--compdb={compdb_path}")
    if base_dir:
        cmd.append(f"--base-dir={base_dir}")
    if sarif_out:
        cmd.append(f"--sarif-out={sarif_out}")
    cmd.extend(extra_args)
    return cmd


def main() -> int:
    args = parse_args()

    analyzer = norm(Path(args.analyzer))
    if not analyzer.exists():
        print(f"Analyzer binary not found: {analyzer}", file=sys.stderr)
        return 2

    repo_root = norm(Path(args.repo_root))
    cwd = norm(Path.cwd())

    selected_inputs: list[Path] = []
    for input_arg in args.inputs:
        input_path = Path(input_arg)
        if not input_path.is_absolute():
            input_path = cwd / input_path
        selected_inputs.append(norm(input_path))

    if args.inputs_from_git:
        try:
            selected_inputs.extend(discover_inputs_from_git(repo_root))
        except Exception as exc:
            print(f"Failed to discover inputs from git: {exc}", file=sys.stderr)
            return 2

    compdb_path = None
    compdb_selected_files: list[Path] = []
    if args.compdb:
        try:
            compdb_path, entries = load_compdb(args.compdb)
            ensure_compdb_compat_symlink(compdb_path, entries, repo_root)
            compdb_selected_files = compdb_files(compdb_path, entries)
        except Exception as exc:
            print(f"Failed to load compile_commands.json: {exc}", file=sys.stderr)
            return 2

    selected_inputs = dedupe(selected_inputs)
    if selected_inputs and compdb_selected_files:
        compdb_file_set = {str(path) for path in compdb_selected_files}
        selected_inputs = [path for path in selected_inputs if str(path) in compdb_file_set]
    elif not selected_inputs and compdb_selected_files:
        selected_inputs = compdb_selected_files

    if args.exclude:
        selected_inputs = [
            path for path in selected_inputs if not is_excluded(path, repo_root, args.exclude)
        ]

    unsupported_count = sum(1 for path in selected_inputs if not is_supported_source(path))
    if unsupported_count:
        print(
            f"Skipping {unsupported_count} unsupported input file(s) "
            f"(supported: {', '.join(sorted(SUPPORTED_SUFFIXES))})."
        )
    selected_inputs = [path for path in selected_inputs if is_supported_source(path)]

    selected_inputs = [path for path in dedupe(selected_inputs) if path.exists() and path.is_file()]
    if not selected_inputs:
        print("No input files selected.", file=sys.stderr)
        return 2

    sarif_out_path: str | None = None
    if args.sarif_out:
        ensure_parent(Path(args.sarif_out))
        sarif_out_path = str(Path(args.sarif_out).resolve())

    print(f"Running analyzer on {len(selected_inputs)} file(s).")
    cmd = analyzer_cmd(
        analyzer=analyzer,
        inputs=selected_inputs,
        fmt="json",
        compdb_path=compdb_path,
        base_dir=args.base_dir,
        extra_args=args.analyzer_arg,
        sarif_out=sarif_out_path,
    )
    run = subprocess.run(cmd, check=False, capture_output=True, text=True)
    if run.returncode != 0:
        if run.stdout:
            sys.stdout.write(run.stdout)
        if run.stderr:
            sys.stderr.write(run.stderr)
        return run.returncode

    try:
        payload = json.loads(run.stdout)
    except json.JSONDecodeError as exc:
        print(f"Analyzer returned invalid JSON: {exc}", file=sys.stderr)
        return 2

    if args.json_out:
        json_output_path = Path(args.json_out)
        ensure_parent(json_output_path)
        json_output_path.write_text(run.stdout, encoding="utf-8")

    diags = payload.get("diagnostics", [])
    if not isinstance(diags, list):
        diags = []

    errors = sum(1 for d in diags if sev(d) == "ERROR")
    warnings = sum(1 for d in diags if sev(d) == "WARNING")
    infos = sum(1 for d in diags if sev(d) == "INFO")
    print(f"Findings summary: errors={errors}, warnings={warnings}, infos={infos}")

    print_diags(diags, args.print_diagnostics)

    failed = (args.fail_on == "error" and errors > 0) or (
        args.fail_on == "warning" and (errors > 0 or warnings > 0)
    )
    if failed:
        print(f"CI gate failed (fail-on={args.fail_on}).", file=sys.stderr)
        return 1

    print(f"CI gate passed (fail-on={args.fail_on}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
