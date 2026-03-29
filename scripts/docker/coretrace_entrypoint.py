#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import argparse
import json
import os
import pathlib
import sys
from typing import Iterable, Optional


ANALYZER_BIN = os.getenv("CORETRACE_ANALYZER_BIN", "/usr/local/bin/stack_usage_analyzer")
WORKSPACE = os.getenv("CORETRACE_WORKSPACE", "/workspace")
DEFAULT_PROFILE = os.getenv("CORETRACE_ANALYSIS_PROFILE", "fast")
DEFAULT_MODEL = os.getenv(
    "CORETRACE_RESOURCE_MODEL", "/models/resource-lifetime/generic.txt"
)
DEFAULT_COMPILE_COMMANDS = os.getenv("CORETRACE_COMPILE_COMMANDS", "")
DEFAULT_COMPDB_FAST = os.getenv("CORETRACE_COMPDB_FAST", "1")
DEFAULT_COMPAT_SYMLINK_ALLOWED_ROOTS = os.getenv(
    "CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS", "/tmp:/var/tmp"
)


def log(message: str) -> None:
    print(f"[coretrace-docker] {message}", file=sys.stderr)


def has_option_prefix(prefix: str, argv: Iterable[str]) -> bool:
    pref_eq = f"{prefix}="
    return any(arg == prefix or arg.startswith(pref_eq) for arg in argv)


def has_any_compile_db_option(argv: Iterable[str]) -> bool:
    return has_option_prefix("--compile-commands", argv) or has_option_prefix(
        "--compdb", argv
    )


def is_truthy(value: str) -> bool:
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _find_compile_commands(workspace: pathlib.Path, max_depth: int = 5) -> Optional[str]:
    try:
        workspace = workspace.resolve()
    except OSError:
        return None
    base_depth = len(workspace.parts)
    for root, dirs, files in os.walk(workspace):
        root_path = pathlib.Path(root)
        depth = len(root_path.parts) - base_depth
        if depth >= max_depth:
            dirs[:] = []
        if "compile_commands.json" in files:
            return str(root_path / "compile_commands.json")
    return None


def _parse_compat_symlink_allowed_roots() -> list[pathlib.Path]:
    roots: list[pathlib.Path] = []
    raw_value = os.getenv(
        "CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS", DEFAULT_COMPAT_SYMLINK_ALLOWED_ROOTS
    )
    for token in raw_value.split(":"):
        candidate = token.strip()
        if not candidate:
            continue
        path = pathlib.Path(candidate)
        if not path.is_absolute():
            log(f"ignoring non-absolute allowed symlink root: {candidate}")
            continue
        try:
            roots.append(path.resolve(strict=False))
        except OSError:
            roots.append(path)
    return roots


def _is_within_root(path: pathlib.Path, root: pathlib.Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def resolve_compdb_path() -> Optional[str]:
    candidate = DEFAULT_COMPILE_COMMANDS
    if candidate:
        candidate_path = pathlib.Path(candidate)
        if candidate_path.is_dir():
            candidate_path = candidate_path / "compile_commands.json"
        if candidate_path.is_file():
            return str(candidate_path)
        log(f"CORETRACE_COMPILE_COMMANDS is set but not found: {candidate_path}")
        return None

    workspace = pathlib.Path(WORKSPACE)
    preferred = workspace / "build" / "compile_commands.json"
    if preferred.is_file():
        return str(preferred)

    fallback = workspace / "compile_commands.json"
    if fallback.is_file():
        return str(fallback)

    found = _find_compile_commands(workspace, max_depth=5)
    if found:
        return found
    return None


def ensure_compdb_compat_symlink(compdb_path: str) -> None:
    compdb = pathlib.Path(compdb_path)
    workspace = pathlib.Path(WORKSPACE)

    try:
        data = json.loads(compdb.read_text(encoding="utf-8"))
    except Exception:
        return

    if not isinstance(data, list) or not data:
        return

    entry_dir: Optional[pathlib.Path] = None
    for entry in data:
        if not isinstance(entry, dict):
            continue
        directory = entry.get("directory")
        if isinstance(directory, str) and directory.startswith("/"):
            entry_dir = pathlib.Path(directory)
            break

    if entry_dir is None:
        return

    if entry_dir.exists():
        return

    try:
        compdb_build_dir = compdb.parent.resolve()
    except OSError:
        return

    if entry_dir.name != compdb_build_dir.name:
        return

    legacy_root = entry_dir.parent
    legacy_root_s = str(legacy_root)
    if legacy_root_s in {"/", ""}:
        return

    try:
        legacy_root_resolved = legacy_root.resolve(strict=False)
    except OSError:
        legacy_root_resolved = legacy_root
    allowed_roots = _parse_compat_symlink_allowed_roots()
    if not allowed_roots:
        log("compatibility symlink allowlist is empty; refusing symlink creation")
        return
    if not any(_is_within_root(legacy_root_resolved, root) for root in allowed_roots):
        log(
            "refusing compatibility symlink outside allowlist roots: "
            f"{legacy_root} (allowlist: {', '.join(str(root) for root in allowed_roots)})"
        )
        return
    if any(legacy_root_resolved == root for root in allowed_roots):
        log(f"refusing compatibility symlink on allowlist root itself: {legacy_root_resolved}")
        return

    try:
        if legacy_root.resolve() == workspace.resolve():
            return
    except OSError:
        pass

    if legacy_root.exists() or legacy_root.is_symlink():
        return

    legacy_parent = legacy_root.parent
    try:
        legacy_parent.mkdir(parents=True, exist_ok=True)
    except Exception:
        log(f"unable to create compatibility parent dir: {legacy_parent}")
        return

    try:
        legacy_root.symlink_to(workspace)
        log(f"created compatibility symlink: {legacy_root} -> {workspace}")
    except Exception:
        log("unable to create compatibility symlink for compile_commands absolute paths")


def exec_analyzer(argv: list[str]) -> int:
    try:
        os.execvp(argv[0], argv)
    except FileNotFoundError:
        log(f"analyzer executable not found: {argv[0]}")
        return 127
    except OSError as exc:
        log(f"failed to execute analyzer '{argv[0]}': {exc}")
        return 126
    return 0


def parse_wrapper_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Bypass wrapper defaults and forward all remaining arguments as-is.",
    )
    parser.add_argument(
        "-h",
        "--help",
        dest="help_requested",
        action="store_true",
        help="Forward analyzer help.",
    )
    return parser.parse_known_args(argv)


def strip_first_arg(argv: list[str], value: str) -> list[str]:
    stripped: list[str] = []
    removed = False
    for arg in argv:
        if not removed and arg == value:
            removed = True
            continue
        stripped.append(arg)
    return stripped


def main() -> int:
    args = sys.argv[1:]
    wrapper_args, _ = parse_wrapper_args(args)

    if wrapper_args.raw:
        forward_args = strip_first_arg(args, "--raw")
        return exec_analyzer([ANALYZER_BIN, *forward_args])

    if wrapper_args.help_requested:
        return exec_analyzer([ANALYZER_BIN, *args])

    cmd: list[str] = [ANALYZER_BIN]
    using_compdb = False

    if not has_option_prefix("--analysis-profile", args):
        cmd.append(f"--analysis-profile={DEFAULT_PROFILE}")

    if not has_any_compile_db_option(args):
        compdb_path = resolve_compdb_path()
        if compdb_path is None:
            log(f"compile_commands.json not found under {WORKSPACE}.")
            log("Generate it first (e.g. CMake: -DCMAKE_EXPORT_COMPILE_COMMANDS=ON).")
            log("Or provide --compile-commands=... (or --compdb=...) explicitly.")
            return 2
        ensure_compdb_compat_symlink(compdb_path)
        cmd.append(f"--compile-commands={compdb_path}")
        using_compdb = True
    else:
        using_compdb = True

    if (
        using_compdb
        and not has_option_prefix("--compdb-fast", args)
        and is_truthy(DEFAULT_COMPDB_FAST)
    ):
        cmd.append("--compdb-fast")

    if not has_option_prefix("--resource-model", args):
        model = pathlib.Path(DEFAULT_MODEL)
        if model.is_file():
            cmd.append(f"--resource-model={model}")
        else:
            log(f"default resource model not found: {model}")
            log("continue without default --resource-model (pass one explicitly if needed)")

    if not has_option_prefix("--resource-summary-cache-memory-only", args) and not has_option_prefix(
        "--resource-summary-cache-dir", args
    ):
        cmd.append("--resource-summary-cache-memory-only")



    cmd.extend(args)
    return exec_analyzer(cmd)


if __name__ == "__main__":
    raise SystemExit(main())
