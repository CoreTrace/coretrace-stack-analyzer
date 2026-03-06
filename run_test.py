#!/usr/bin/env python3
import argparse
import contextlib
import importlib.util
import io
import shlex
import sys
import subprocess
import json
import re
import hashlib
import os
import shutil
import threading
import tempfile
import uuid
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Optional

DEFAULT_ANALYZER = Path("./build/stack_usage_analyzer")
DEFAULT_TEST_DIR = Path("test")
DEFAULT_CACHE_DIR = Path(".cache/run_test")


@dataclass
class TestRunConfig:
    analyzer: Path = DEFAULT_ANALYZER
    test_dir: Path = DEFAULT_TEST_DIR
    cache_dir: Path = DEFAULT_CACHE_DIR
    jobs: int = 1
    cache_enabled: bool = True
    extra_analyzer_args: tuple[str, ...] = ()


RUN_CONFIG = TestRunConfig()
_CACHE_LOCK = threading.Lock()
_MEM_CACHE = {}
# Set to True while the top-level parallel check phase is running.
# Prevents nested ThreadPoolExecutor creation (N² process explosion).
_PARALLEL_PHASE = False

# Pre-compiled regex patterns for hot paths
_RE_LOCATION = re.compile(r"\s*at line (\d+), column (\d+)\s*$")
_RE_LOCATION_STRICT = re.compile(r"^at line \d+, column \d+$")
_RE_FORTIFIED = re.compile(r"__([A-Za-z0-9_]+)_chk\b")
_RE_HEADLINE_WARN = re.compile(r"^\[\s*!{2}Warn\s*\]\s+.+$", flags=re.IGNORECASE)
_RE_HEADLINE_ERR = re.compile(r"^\[\s*!{2}Err\s*\]\s+.+$", flags=re.IGNORECASE)
_RE_HEADLINE_ERROR = re.compile(r"^\[\s*!{3}Error\s*\]\s+.+$", flags=re.IGNORECASE)
_RE_HEADLINE_LEGACY = re.compile(r"^\[\s*!{2}\s*\]\s+.+$")
_RE_DIAG_SUMMARY = re.compile(
    r"^Diagnostics summary:\s*info=(\d+),\s*warning=(\d+),\s*error=(\d+)\s*$",
    flags=re.MULTILINE,
)
_RE_STACK_LIMIT = re.compile(r"//\s*stack-limit\s*[:=]\s*(\S+)", re.IGNORECASE)
_RE_RESOURCE_MODEL = re.compile(r"//\s*resource-model\s*[:=]\s*(\S+)", re.IGNORECASE)
_RE_ESCAPE_MODEL = re.compile(r"//\s*escape-model\s*[:=]\s*(\S+)", re.IGNORECASE)
_RE_BUFFER_MODEL = re.compile(r"//\s*buffer-model\s*[:=]\s*(\S+)", re.IGNORECASE)
_RE_STRICT_DIAG = re.compile(r"//\s*strict-diagnostic-count\s*[:=]\s*(\S+)", re.IGNORECASE)


# Thread-safe stdout dispatcher for parallel check execution
class _ThreadDispatchStdout:
    """Route print() output to per-thread buffers when in parallel mode."""

    def __init__(self, original):
        self._original = original
        self._buffers: dict[int, io.StringIO] = {}

    def register_thread(self):
        self._buffers[threading.get_ident()] = io.StringIO()

    def unregister_thread(self) -> str:
        buf = self._buffers.pop(threading.get_ident(), None)
        return buf.getvalue() if buf else ""

    def write(self, s):
        buf = self._buffers.get(threading.get_ident())
        if buf is not None:
            return buf.write(s)
        return self._original.write(s)

    def flush(self):
        buf = self._buffers.get(threading.get_ident())
        if buf is None:
            self._original.flush()

    def __getattr__(self, name):
        return getattr(self._original, name)


def is_fixture_source(path: Path) -> bool:
    """
    Return True if this source file should be analyzed as a regression fixture.
    """
    try:
        rel = path.resolve().relative_to(RUN_CONFIG.test_dir.resolve())
    except Exception:
        rel = path
    return not (len(rel.parts) > 0 and rel.parts[0] == "unit")


def collect_fixture_sources():
    """
    Collect C/C++ fixtures under test/, excluding helper/unit-test sources.
    """
    fixture_sources = []
    for pattern in ("**/*.c", "**/*.cc", "**/*.cpp", "**/*.cxx"):
        fixture_sources.extend(RUN_CONFIG.test_dir.glob(pattern))
    return [path for path in sorted(fixture_sources) if is_fixture_source(path)]


def parse_args():
    parser = argparse.ArgumentParser(
        description="Run analyzer regression tests with optional parallelism and caching."
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="Number of worker threads for test parallelism: global checks, "
             "per-file fixture checks, and parity checks all run concurrently (default: 1).",
    )
    parser.add_argument(
        "--cache-dir",
        default=str(RUN_CONFIG.cache_dir),
        help="Directory used for analyzer output cache (default: .cache/run_test).",
    )
    parser.add_argument(
        "--no-cache",
        action="store_true",
        help="Disable analyzer output cache.",
    )
    parser.add_argument(
        "--clear-cache",
        action="store_true",
        help="Delete cache directory before running tests.",
    )
    parser.add_argument(
        "--analyzer-arg",
        action="append",
        default=[],
        help=(
            "Extra argument forwarded to analyzer invocations that process source inputs. "
            "Repeatable."
        ),
    )
    return parser.parse_args()


def _collect_cache_dependencies(args):
    deps = []
    candidates = {Path(__file__).resolve(), RUN_CONFIG.analyzer.resolve()}
    for arg in args:
        if arg.startswith("-") and "=" in arg:
            value = arg.split("=", 1)[1]
            if value:
                p = Path(value)
                if p.exists():
                    candidates.add(p.resolve())
            continue

        if arg.startswith("-"):
            continue

        p = Path(arg)
        if p.exists():
            candidates.add(p.resolve())

    for p in sorted(candidates, key=lambda x: str(x)):
        try:
            st = p.stat()
        except OSError:
            continue
        deps.append([str(p), st.st_mtime_ns, st.st_size])
    return deps


def _cache_key_for_args(args):
    payload = {
        "analyzer": str(RUN_CONFIG.analyzer.resolve()),
        "args": list(args),
        "cwd": str(Path.cwd()),
        "deps": _collect_cache_dependencies(args),
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _cache_path_for_key(key):
    return RUN_CONFIG.cache_dir / f"{key}.json"


def _cache_load(key):
    if not RUN_CONFIG.cache_enabled:
        return None
    cache_path = _cache_path_for_key(key)
    if not cache_path.exists():
        return None
    try:
        data = json.loads(cache_path.read_text(encoding="utf-8"))
    except Exception:
        return None
    return subprocess.CompletedProcess(
        args=data.get("args", []),
        returncode=int(data.get("returncode", 1)),
        stdout=data.get("stdout", ""),
        stderr=data.get("stderr", ""),
    )


def _cache_store(key, result):
    if not RUN_CONFIG.cache_enabled:
        return
    try:
        RUN_CONFIG.cache_dir.mkdir(parents=True, exist_ok=True)
        cache_path = _cache_path_for_key(key)
        tmp_path = cache_path.with_suffix(
            f"{cache_path.suffix}.{os.getpid()}.{threading.get_ident()}.tmp"
        )
        tmp_path.write_text(
            json.dumps(
                {
                    "args": result.args,
                    "returncode": result.returncode,
                    "stdout": result.stdout or "",
                    "stderr": result.stderr or "",
                },
                ensure_ascii=True,
            ),
            encoding="utf-8",
        )
        tmp_path.replace(cache_path)
    except Exception:
        # Cache failures must not fail tests.
        pass


def normalize(s: str) -> str:
    """
    Normalize spacing to make comparisons more robust:
    - remove unnecessary leading/trailing spaces per line
    - replace runs of spaces with a single space
    - keep line breaks
    """
    lines = []
    for line in s.splitlines():
        line = line.rstrip("\n")
        # "a   b   c" -> "a b c"
        parts = line.strip().split()
        normalized = " ".join(parts)
        # Normalize spacing around pointer/reference symbols for cross-platform demangler output.
        normalized = normalized.replace(" *", "*").replace("* ", "*")
        normalized = normalized.replace(" &", "&").replace("& ", "&")
        # Normalize fortified libc function names (e.g., "__strncpy_chk" -> "strncpy").
        normalized = _RE_FORTIFIED.sub(r"\1", normalized)
        lines.append(normalized)
    return "\n".join(lines).strip()


def _location_tolerant_variants(expectation: str) -> list[str]:
    """
    Build location-tolerant expectation variants for common source drift in
    "at line X, column Y" headers (formatting refactors, brace style changes,
    toolchain column shifts).
    """
    lines = expectation.splitlines()
    if not lines:
        return []
    match = _RE_LOCATION.match(lines[0])
    if not match:
        return []
    line = int(match.group(1))
    column = int(match.group(2))
    variants: list[str] = []
    # Keep tolerance small enough to catch wrong/stale expectations, while
    # still absorbing routine formatting drift.
    max_line_delta = 18
    for line_delta in range(-max_line_delta, max_line_delta + 1):
        for col_delta in (-2, -1, 0, 1, 2):
            if line_delta == 0 and col_delta == 0:
                continue
            candidate_line = line + line_delta
            candidate_column = column + col_delta
            if candidate_line <= 0 or candidate_column < 0:
                continue
            alt_lines = list(lines)
            alt_lines[0] = f"at line {candidate_line}, column {candidate_column}"
            variants.append("\n".join(alt_lines))
    return variants


def extract_expectations(c_path: Path):
    """
    Extract expected comment blocks from a .c file.

    Look for comments that start with "// at line" and take all following comment lines.
    """
    expectations = []
    negative_expectations = []
    stack_limit = None
    resource_model = None
    escape_model = None
    buffer_model = None
    strict_diag_count = None
    lines = c_path.read_text().splitlines()
    i = 0
    n = len(lines)

    def parse_bool_directive(value: str):
        token = value.strip().lower()
        if token in {"1", "true", "yes", "on"}:
            return True
        if token in {"0", "false", "no", "off"}:
            return False
        return None

    while i < n:
        raw = lines[i]
        stripped = raw.lstrip()

        stack_match = _RE_STACK_LIMIT.match(stripped)
        if stack_match:
            stack_limit = stack_match.group(1)
            i += 1
            continue
        resource_match = _RE_RESOURCE_MODEL.match(stripped)
        if resource_match:
            resource_model = resource_match.group(1)
            i += 1
            continue
        escape_match = _RE_ESCAPE_MODEL.match(stripped)
        if escape_match:
            escape_model = escape_match.group(1)
            i += 1
            continue
        buffer_match = _RE_BUFFER_MODEL.match(stripped)
        if buffer_match:
            buffer_model = buffer_match.group(1)
            i += 1
            continue
        strict_match = _RE_STRICT_DIAG.match(stripped)
        if strict_match:
            parsed = parse_bool_directive(strict_match.group(1))
            if parsed is not None:
                strict_diag_count = parsed
            i += 1
            continue

        stripped_line = stripped
        if stripped_line.startswith("// not contains:"):
            negative = stripped_line[len("// not contains:"):].strip()
            if negative:
                negative_expectations.append(negative)
            i += 1
            continue

        # Start of an expectation block
        if stripped.startswith("// at line"):
            comment_block = [raw]
            i += 1
            # Collect all following "// ..." lines
            while i < n and lines[i].lstrip().startswith("//"):
                comment_block.append(lines[i])
                i += 1

            # Cleanup: remove "//" and indentation
            cleaned_lines = []
            for c in comment_block:
                s = c.lstrip()
                if s.startswith("//"):
                    s = s[2:]  # remove "//"
                cleaned_lines.append(s.lstrip())

            expectation_text = "\n".join(cleaned_lines)
            expectations.append(expectation_text)
        else:
            i += 1

    return (
        expectations,
        negative_expectations,
        stack_limit,
        resource_model,
        escape_model,
        buffer_model,
        strict_diag_count,
    )


def _expectation_is_warning_or_error(expectation: str) -> bool:
    norm = normalize(expectation).lower()
    if "[" not in norm:
        # Keep unknown legacy style expectations conservative.
        return True
    if "error" in norm:
        return True
    if "warn" in norm:
        return True
    # Legacy diagnostic style: "[!!] ..."
    if "[!!]" in norm:
        return True
    return False


def _is_diagnostic_headline_line(line: str) -> bool:
    s = normalize(line)
    if not s:
        return False
    if _RE_HEADLINE_WARN.match(s):
        return True
    if _RE_HEADLINE_ERR.match(s):
        return True
    if _RE_HEADLINE_ERROR.match(s):
        return True
    # Legacy terse marker.
    if _RE_HEADLINE_LEGACY.match(s):
        return True
    return False


def _parse_expectation_location_and_headlines(expectation: str):
    lines = [normalize(line) for line in expectation.splitlines() if normalize(line)]
    if not lines:
        return None
    if not _RE_LOCATION_STRICT.match(lines[0]):
        return None
    headlines = [line for line in lines[1:] if _is_diagnostic_headline_line(line)]
    if not headlines:
        return None
    return lines[0], headlines


def _build_output_diagnostic_index_by_location(output: str):
    index: dict[str, list[str]] = {}
    current_location = None
    for raw in output.splitlines():
        line = normalize(raw)
        if not line:
            continue
        if _RE_LOCATION_STRICT.match(line):
            current_location = line
            index.setdefault(current_location, [])
            continue
        if current_location and _is_diagnostic_headline_line(line):
            index[current_location].append(line)
    return index


def _expectation_matches_by_location_and_headlines(expectation: str, output_index) -> bool:
    parsed = _parse_expectation_location_and_headlines(expectation)
    if not parsed:
        return False
    location, headlines = parsed

    location_candidates = {location}
    for alt in _location_tolerant_variants(expectation):
        alt_lines = [normalize(line) for line in alt.splitlines() if normalize(line)]
        if alt_lines and _RE_LOCATION_STRICT.match(alt_lines[0]):
            location_candidates.add(alt_lines[0])

    for candidate in location_candidates:
        observed = output_index.get(candidate, [])
        if all(headline in observed for headline in headlines):
            return True
    return False


def _parse_total_warning_error_count(output: str):
    matches = _RE_DIAG_SUMMARY.findall(output)
    if not matches:
        return None
    _info, warning, error = matches[-1]
    return int(warning) + int(error)


def _default_strict_diagnostic_count(c_path: Path) -> bool:
    """
    Enable strict warning/error count by default for all fixture files.
    Suites can opt-out per-file via: // strict-diagnostic-count: false
    """
    return True


def fixture_path_with_fallback(*relative_candidates: str) -> Path:
    """
    Resolve a fixture path under test/ from a list of relative candidates.
    Returns the first existing candidate, or the first candidate path if none exist.
    """
    if not relative_candidates:
        raise ValueError("fixture_path_with_fallback requires at least one candidate")
    for rel in relative_candidates:
        candidate = RUN_CONFIG.test_dir / rel
        if candidate.exists():
            return candidate
    return RUN_CONFIG.test_dir / relative_candidates[0]


def run_analyzer_on_file(
    c_path: Path,
    stack_limit=None,
    resource_model=None,
    escape_model=None,
    buffer_model=None,
    extra_args: tuple[str, ...] = (),
) -> str:
    """
    Run the analyzer on a C file and capture stdout+stderr.
    """
    args = [str(c_path)]
    if stack_limit:
        args.append(f"--stack-limit={stack_limit}")
    if resource_model:
        args.append(f"--resource-model={resource_model}")
    if escape_model:
        args.append(f"--escape-model={escape_model}")
    if buffer_model:
        args.append(f"--buffer-model={buffer_model}")
    if extra_args:
        args.extend(extra_args)
    result = run_analyzer(args)
    output = (result.stdout or "") + (result.stderr or "")
    return output


def _runner_has_explicit_smt_args() -> bool:
    for arg in RUN_CONFIG.extra_analyzer_args:
        if arg.startswith("--smt"):
            return True
    return False


def _all_smt_rules() -> tuple[str, ...]:
    """
    Rules currently integrated with SMT refinement.
    Applied to all fixture files for the dedicated SMT+Z3 pass.
    """
    return (
        "recursion",
        "integer-overflow",
        "size-minus-k",
        "stack-buffer",
        "oob-read",
    )


def _has_positional_input_arg(args) -> bool:
    """
    Return True when args appear to include at least one positional input path.
    """
    for arg in args:
        if not arg.startswith("-"):
            return True
    return False


def _effective_analyzer_args(args):
    """
    Merge optional runner-level analyzer args for invocations that analyze inputs.
    Keep runner-provided compile overrides at the end so they have highest
    precedence against compile database flags and per-check compile args.
    """
    base = list(args)
    if RUN_CONFIG.extra_analyzer_args and _has_positional_input_arg(base):
        prefix_args = []
        trailing_compile_override_args = []
        extras = list(RUN_CONFIG.extra_analyzer_args)
        i = 0
        while i < len(extras):
            token = extras[i]
            if token == "--compile-arg":
                trailing_compile_override_args.append(token)
                if i + 1 < len(extras):
                    trailing_compile_override_args.append(extras[i + 1])
                    i += 2
                    continue
                i += 1
                continue
            if token.startswith("--compile-arg="):
                trailing_compile_override_args.append(token)
                i += 1
                continue
            prefix_args.append(token)
            i += 1

        return [*prefix_args, *base, *trailing_compile_override_args]
    return base


def run_analyzer(args) -> subprocess.CompletedProcess:
    """
    Run analyzer with custom args and return the CompletedProcess.
    """
    effective_args = _effective_analyzer_args(args)
    cmd = [str(RUN_CONFIG.analyzer)] + effective_args
    key = _cache_key_for_args(effective_args)

    with _CACHE_LOCK:
        in_memory = _MEM_CACHE.get(key)
    if in_memory is not None:
        return subprocess.CompletedProcess(
            args=cmd,
            returncode=in_memory["returncode"],
            stdout=in_memory["stdout"],
            stderr=in_memory["stderr"],
        )

    cached = _cache_load(key)
    if cached is not None:
        cached.args = cmd
        with _CACHE_LOCK:
            _MEM_CACHE[key] = {
                "returncode": cached.returncode,
                "stdout": cached.stdout or "",
                "stderr": cached.stderr or "",
            }
        return cached

    result = subprocess.run(cmd, capture_output=True, text=True)
    with _CACHE_LOCK:
        _MEM_CACHE[key] = {
            "returncode": result.returncode,
            "stdout": result.stdout or "",
            "stderr": result.stderr or "",
        }
    _cache_store(key, result)
    return result


def run_analyzer_uncached(args) -> subprocess.CompletedProcess:
    """
    Run analyzer with custom args and bypass run_test.py cache layer.
    Useful for checks that assert filesystem side effects.
    """
    cmd = [str(RUN_CONFIG.analyzer)] + _effective_analyzer_args(args)
    return subprocess.run(cmd, capture_output=True, text=True)


def fail_check(message: str, output: str = "") -> bool:
    print(f"  ❌ {message}")
    if output:
        print(output)
    print()
    return False


def expect_returncode_zero(result: subprocess.CompletedProcess, output: str, context: str) -> bool:
    if result.returncode == 0:
        return True
    return fail_check(f"{context} (code {result.returncode})", output)


def expect_contains(output: str, needle: str, context: str) -> bool:
    if needle in output:
        return True
    return fail_check(context, output)


def expect_not_contains(output: str, needle: str, context: str) -> bool:
    if needle not in output:
        return True
    return fail_check(context, output)


def load_docker_entrypoint_module():
    entrypoint_path = Path("scripts/docker/coretrace_entrypoint.py")
    if not entrypoint_path.exists():
        return None, f"entrypoint script not found: {entrypoint_path}"
    module_name = f"coretrace_entrypoint_test_{uuid.uuid4().hex}"
    spec = importlib.util.spec_from_file_location(module_name, entrypoint_path)
    if spec is None or spec.loader is None:
        return None, f"unable to load module spec: {entrypoint_path}"
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module, ""


def parse_stack_line(line: str, label: str):
    """
    Parse stack lines like:
      "local stack: 123 bytes"
      "local stack: unknown (>= 256 bytes)"
    Returns dict with unknown/value/lower_bound or None if not matched.
    """
    label_re = re.escape(label)
    m_unknown = re.search(
        rf"{label_re}:\s*unknown(?:\s*\(>=\s*(\d+)\s*bytes\))?", line
    )
    if m_unknown:
        lower_bound = int(m_unknown.group(1)) if m_unknown.group(1) else None
        return {"unknown": True, "value": None, "lower_bound": lower_bound}
    m_value = re.search(rf"{label_re}:\s*(\d+)\s*bytes", line)
    if m_value:
        return {"unknown": False, "value": int(m_value.group(1)), "lower_bound": None}
    return None


def parse_human_functions(output: str):
    """
    Parse human-readable output to extract per-function metadata.
    """
    functions = {}
    lines = output.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.startswith("Function: "):
            i += 1
            continue

        # Detect the end of this function block.
        j = i + 1
        while j < len(lines) and not lines[j].startswith("Function: "):
            if lines[j].startswith("Mode: ") or lines[j].startswith("File: "):
                break
            j += 1

        block = lines[i:j]
        if any(l.strip().startswith("local stack:") for l in block):
            if "(line " in line:
                i = j
                continue
            rest = line[len("Function: "):].strip()
            if rest:
                name = rest.split()[0]
                functions[name] = {
                    "localStackUnknown": None,
                    "localStack": None,
                    "localStackLowerBound": None,
                    "maxStackUnknown": None,
                    "maxStack": None,
                    "maxStackLowerBound": None,
                    "isRecursive": False,
                    "hasInfiniteSelfRecursion": False,
                    "exceedsLimit": False,
                }

                summary_lines = []
                for block_line in block[1:]:
                    stripped = block_line.strip()
                    if stripped.startswith("at line "):
                        break
                    summary_lines.append(block_line)

                for block_line in summary_lines:
                    stripped = block_line.strip()
                    if stripped.startswith("local stack:"):
                        info = parse_stack_line(stripped, "local stack")
                        if info:
                            functions[name]["localStackUnknown"] = info["unknown"]
                            functions[name]["localStack"] = info["value"]
                            functions[name]["localStackLowerBound"] = info["lower_bound"]
                    elif stripped.startswith("max stack (including callees):"):
                        info = parse_stack_line(stripped, "max stack (including callees)")
                        if info:
                            functions[name]["maxStackUnknown"] = info["unknown"]
                            functions[name]["maxStack"] = info["value"]
                            functions[name]["maxStackLowerBound"] = info["lower_bound"]

                # Recursion diagnostics may appear either directly in the summary
                # or below an "at line ..." location line.
                for block_line in block[1:]:
                    stripped = block_line.strip()
                    if "recursive or mutually recursive function detected" in stripped:
                        functions[name]["isRecursive"] = True
                    elif "unconditional self recursion detected" in stripped:
                        functions[name]["hasInfiniteSelfRecursion"] = True

                # Stack overflow diagnostics can appear after a location line.
                for block_line in block[1:]:
                    if "potential stack overflow: exceeds limit of" in block_line:
                        functions[name]["exceedsLimit"] = True
                        break

        i = j
    return functions


def extract_human_function_block(output: str, func_name: str):
    """
    Return the human-readable block for a given function name, if present.
    """
    lines = output.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith("Function: "):
            rest = line[len("Function: "):].strip()
            if rest and rest.split()[0] == func_name:
                # Capture until next Function/Mode/File header.
                j = i + 1
                while j < len(lines):
                    if lines[j].startswith(("Function: ", "Mode: ", "File: ")):
                        break
                    j += 1
                return "\n".join(lines[i:j]).strip()
        i += 1
    return ""


def parse_human_diagnostic_messages(output: str):
    """
    Extract diagnostic message blocks from human-readable output.
    """
    blocks = []
    lines = output.splitlines()
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if stripped.startswith("Function:") and "(line " in stripped:
            # Diagnostic blocks that start with a Function: header line.
            block_lines = [lines[i]]
            i += 1
            while i < len(lines):
                next_line = lines[i]
                next_stripped = next_line.strip()
                if next_stripped == "":
                    break
                if next_stripped.startswith(("Function:", "Mode:", "File:")):
                    break
                if next_stripped.startswith(("local stack:", "max stack (including callees):")):
                    break
                if next_stripped.startswith("[") and not next_line[:1].isspace():
                    break
                block_lines.append(next_line)
                i += 1

            blocks.append(normalize("\n".join(block_lines)))

            if i < len(lines) and lines[i].strip() == "":
                i += 1
            continue

        if stripped.startswith("at line ") and ", column " in stripped:
            # Diagnostic blocks that follow a source location line.
            block_lines = []
            i += 1
            while i < len(lines):
                next_line = lines[i]
                next_stripped = next_line.strip()
                if next_stripped == "":
                    break
                if next_stripped.startswith(("Function:", "Mode:", "File:")):
                    break
                if next_stripped.startswith(("local stack:", "max stack (including callees):")):
                    break
                if next_stripped.startswith("[") and not next_line[:1].isspace():
                    break
                block_lines.append(next_line)
                i += 1

            if block_lines:
                blocks.append(normalize("\n".join(block_lines)))

            if i < len(lines) and lines[i].strip() == "":
                i += 1
            continue

        if stripped.startswith("[!") or stripped.startswith("[!!]") or stripped.startswith("[!!!]"):
            # Diagnostic blocks that appear without an explicit location line.
            block_lines = [lines[i]]
            i += 1
            while i < len(lines):
                next_line = lines[i]
                next_stripped = next_line.strip()
                if next_stripped == "":
                    break
                if next_stripped.startswith(("Function:", "Mode:", "File:")):
                    break
                if next_stripped.startswith(("local stack:", "max stack (including callees):")):
                    break
                if next_stripped.startswith("[") and not next_line[:1].isspace():
                    break
                block_lines.append(next_line)
                i += 1
            blocks.append(normalize("\n".join(block_lines)))
            if i < len(lines) and lines[i].strip() == "":
                i += 1
            continue

        i += 1

    return blocks


def _check_human_vs_json_parity_sample(sample: Path):
    lines = []
    sample_ok = True

    human = run_analyzer([str(sample)])
    if human.returncode != 0:
        lines.append(f"  ❌ human run failed for {sample} (code {human.returncode})")
        lines.append(human.stdout or "")
        lines.append(human.stderr or "")
        return False, "\n".join(lines).rstrip() + "\n"

    structured = run_analyzer([str(sample), "--format=json"])
    if structured.returncode != 0:
        lines.append(f"  ❌ json run failed for {sample} (code {structured.returncode})")
        lines.append(structured.stdout or "")
        lines.append(structured.stderr or "")
        return False, "\n".join(lines).rstrip() + "\n"

    try:
        payload = json.loads(structured.stdout)
    except json.JSONDecodeError as exc:
        lines.append(f"  ❌ invalid JSON output for {sample}: {exc}")
        lines.append(structured.stdout or "")
        return False, "\n".join(lines).rstrip() + "\n"

    human_output = (human.stdout or "") + (human.stderr or "")
    norm_human = normalize(human_output)
    human_functions = parse_human_functions(human_output)
    human_diag_blocks = parse_human_diagnostic_messages(human_output)

    mode = payload.get("meta", {}).get("mode")
    if mode and f"Mode: {mode}" not in human_output:
        lines.append(f"  ❌ mode mismatch for {sample} (json={mode})")
        sample_ok = False

    for f in payload.get("functions", []):
        name = f.get("name", "")
        if not name:
            continue
        if name not in human_functions:
            lines.append(f"  ❌ function missing in human output: {name}")
            sample_ok = False
            continue
        hf = human_functions[name]

        if hf["localStackUnknown"] is None:
            lines.append(f"  ❌ local stack missing in human output for: {name}")
            sample_ok = False
        elif f.get("localStackUnknown") != hf["localStackUnknown"]:
            lines.append(f"  ❌ local stack unknown flag mismatch for: {name}")
            sample_ok = False
        elif not f.get("localStackUnknown"):
            if f.get("localStack") != hf["localStack"]:
                lines.append(f"  ❌ local stack value mismatch for: {name}")
                sample_ok = False
        elif hf["localStackLowerBound"] is not None:
            json_lb = f.get("localStackLowerBound")
            if json_lb != hf["localStackLowerBound"]:
                lines.append(f"  ❌ local stack lower bound mismatch for: {name}")
                sample_ok = False

        if hf["maxStackUnknown"] is None:
            lines.append(f"  ❌ max stack missing in human output for: {name}")
            sample_ok = False
        elif f.get("maxStackUnknown") != hf["maxStackUnknown"]:
            lines.append(f"  ❌ max stack unknown flag mismatch for: {name}")
            sample_ok = False
        elif not f.get("maxStackUnknown"):
            if f.get("maxStack") != hf["maxStack"]:
                lines.append(f"  ❌ max stack value mismatch for: {name}")
                sample_ok = False
        elif hf["maxStackLowerBound"] is not None:
            json_lb = f.get("maxStackLowerBound")
            if json_lb != hf["maxStackLowerBound"]:
                lines.append(f"  ❌ max stack lower bound mismatch for: {name}")
                sample_ok = False

        if f.get("isRecursive") != hf["isRecursive"]:
            lines.append(f"  ❌ recursion flag mismatch for: {name}")
            lines.append(f"     human: {hf['isRecursive']} json: {f.get('isRecursive')}")
            block = extract_human_function_block(human_output, name)
            if block:
                lines.append("     human block:")
                lines.append(block)
            else:
                lines.append("     human block: <not found>")
            lines.append(f"     json function: {f}")
            # Do not fail on flag mismatch alone; message parity handles recursion info.
        if f.get("hasInfiniteSelfRecursion") != hf["hasInfiniteSelfRecursion"]:
            lines.append(f"  ❌ infinite recursion flag mismatch for: {name}")
            lines.append(
                f"     human: {hf['hasInfiniteSelfRecursion']} json: {f.get('hasInfiniteSelfRecursion')}"
            )
            block = extract_human_function_block(human_output, name)
            if block:
                lines.append("     human block:")
                lines.append(block)
            else:
                lines.append("     human block: <not found>")
            lines.append(f"     json function: {f}")
            # Do not fail on flag mismatch alone; message parity handles recursion info.
        if f.get("exceedsLimit") != hf["exceedsLimit"]:
            lines.append(f"  ❌ stack limit flag mismatch for: {name}")
            lines.append(f"     human: {hf['exceedsLimit']} json: {f.get('exceedsLimit')}")
            block = extract_human_function_block(human_output, name)
            if block:
                lines.append("     human block:")
                lines.append(block)
            else:
                lines.append("     human block: <not found>")
            lines.append(f"     json function: {f}")
            sample_ok = False

    for d in payload.get("diagnostics", []):
        details = d.get("details", {})
        msg = details.get("message", "")
        if msg and normalize(msg) not in norm_human:
            lines.append("  ❌ diagnostic message missing in human output")
            lines.append(f"     message: {msg}")
            sample_ok = False
        loc = d.get("location", {})
        line = loc.get("startLine", 0)
        column = loc.get("startColumn", 0)
        if line and column:
            needle = normalize(f"at line {line}, column {column}")
            if needle not in norm_human:
                lines.append("  ❌ diagnostic location missing in human output")
                lines.append(f"     location: line {line}, column {column}")
                sample_ok = False

    json_messages = {
        normalize(d.get("details", {}).get("message", ""))
        for d in payload.get("diagnostics", [])
        if d.get("details", {}).get("message")
    }
    for block in human_diag_blocks:
        if block and block not in json_messages:
            lines.append("  ❌ diagnostic message missing in JSON output")
            lines.append(f"     message: {block}")
            sample_ok = False

    if sample_ok:
        lines.append(f"  ✅ parity OK for {sample}")
    else:
        lines.append(f"  ❌ parity FAIL for {sample}")

    return sample_ok, "\n".join(lines).rstrip() + "\n"


def check_human_vs_json_parity() -> bool:
    """
    Compare human-readable output vs JSON output for the same input.
    Fails if information present in one view is missing in the other.
    """
    print("=== Testing human vs JSON parity ===")
    samples = collect_fixture_sources()
    if not samples:
        print("  (no .c/.cpp files found, skipping)\n")
        return True

    ok = True
    # When called from the top-level parallel pool, _PARALLEL_PHASE is set
    # so we avoid creating a nested ThreadPoolExecutor (which could cause
    # N² concurrent analyzer processes on constrained runners).
    use_threads = RUN_CONFIG.jobs > 1 and not _PARALLEL_PHASE
    if use_threads:
        with ThreadPoolExecutor(max_workers=RUN_CONFIG.jobs) as executor:
            reports = list(executor.map(_check_human_vs_json_parity_sample, samples))
        for sample_ok, report in reports:
            print(report, end="")
            ok = ok and sample_ok
    else:
        for sample in samples:
            sample_ok, report = _check_human_vs_json_parity_sample(sample)
            print(report, end="")
            ok = ok and sample_ok

    print()
    return ok


def check_help_flags() -> bool:
    """
    Check that -h and --help print help to stdout and return 0.
    """
    print("=== Testing help flags ===")
    ok = True
    for flag in ["-h", "--help"]:
        result = run_analyzer([flag])
        stdout = result.stdout or ""
        if result.returncode != 0:
            print(f"  ❌ {flag} returned {result.returncode} (expected 0)")
            ok = False
            continue
        missing = []
        for needle in ["Stack Usage Analyzer", "Usage:", "Options:", "-h, --help", "Examples:"]:
            if needle not in stdout:
                missing.append(needle)
        if missing:
            print(f"  ❌ {flag} missing help sections: {', '.join(missing)}")
            ok = False
        else:
            print(f"  ✅ {flag} OK")
    print()
    return ok


def check_multi_file_json() -> bool:
    """
    Check that analysis accepts multiple files and JSON aggregates correctly.
    """
    print("=== Testing multi-file JSON ===")
    file_a = RUN_CONFIG.test_dir / "test.ll"
    file_b = RUN_CONFIG.test_dir / "recursion/c/limited-recursion.ll"

    result = run_analyzer([str(file_a), str(file_b), "--format=json"])
    if result.returncode != 0:
        print(f"  ❌ multi-file JSON returned {result.returncode} (expected 0)")
        print(result.stdout)
        print(result.stderr)
        print()
        return False

    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        print(f"  ❌ invalid JSON output: {exc}")
        print(result.stdout)
        print()
        return False

    expected_inputs = sorted([str(file_a), str(file_b)])
    meta = payload.get("meta", {})
    inputs = meta.get("inputFiles", [])
    if inputs != expected_inputs:
        print("  ❌ inputFiles mismatch")
        print(f"     expected: {expected_inputs}")
        print(f"     got:      {inputs}")
        print()
        return False

    functions = payload.get("functions", [])
    function_files = {f.get("file", "") for f in functions if f.get("file", "")}

    def matches_input(input_path: str) -> bool:
        input_stem = Path(input_path).stem
        for fpath in function_files:
            if fpath == input_path:
                return True
            try:
                if Path(fpath).stem == input_stem:
                    return True
            except Exception:
                continue
        return False

    if not all(matches_input(p) for p in expected_inputs):
        print("  ❌ functions missing file attribution for inputs")
        print(f"     expected: {expected_inputs}")
        print(f"     function files: {sorted(function_files)}")
        print()
        return False

    diagnostics = payload.get("diagnostics", [])
    for diag in diagnostics:
        loc = diag.get("location", {})
        if not loc.get("file"):
            print("  ❌ diagnostic missing file attribution")
            print(diag)
            print()
            return False

    print("  ✅ multi-file JSON OK\n")
    return True


def check_multi_file_total_summary() -> bool:
    """
    Check that multi-file human output prints an aggregated diagnostics summary
    and that totals match per-file summaries.
    """
    print("=== Testing multi-file total summary ===")
    file_a = RUN_CONFIG.test_dir / "test.ll"
    file_b = RUN_CONFIG.test_dir / "recursion/c/limited-recursion.ll"
    files = [file_a, file_b]

    result = run_analyzer([str(file_a), str(file_b)])
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        print(f"  ❌ multi-file run failed (code {result.returncode})")
        print(output)
        print()
        return False

    per_file_matches = re.findall(
        r"^Diagnostics summary: info=(\d+), warning=(\d+), error=(\d+)\s*$",
        output,
        flags=re.MULTILINE,
    )
    if len(per_file_matches) != len(files):
        print("  ❌ unexpected number of per-file summaries")
        print(f"     expected: {len(files)} got: {len(per_file_matches)}")
        print(output)
        print()
        return False

    total_match = re.search(
        r"^Total diagnostics summary: info=(\d+), warning=(\d+), error=(\d+) \(across (\d+) files\)\s*$",
        output,
        flags=re.MULTILINE,
    )
    if not total_match:
        print("  ❌ missing total diagnostics summary line")
        print(output)
        print()
        return False

    total_info = int(total_match.group(1))
    total_warning = int(total_match.group(2))
    total_error = int(total_match.group(3))
    total_files = int(total_match.group(4))

    if total_files != len(files):
        print("  ❌ total diagnostics file count mismatch")
        print(f"     expected: {len(files)} got: {total_files}")
        print(output)
        print()
        return False

    sum_info = sum(int(m[0]) for m in per_file_matches)
    sum_warning = sum(int(m[1]) for m in per_file_matches)
    sum_error = sum(int(m[2]) for m in per_file_matches)
    if (total_info, total_warning, total_error) != (sum_info, sum_warning, sum_error):
        print("  ❌ total diagnostics count mismatch")
        print(
            f"     expected: info={sum_info}, warning={sum_warning}, error={sum_error}"
        )
        print(
            f"     got: info={total_info}, warning={total_warning}, error={total_error}"
        )
        print(output)
        print()
        return False

    print("  ✅ multi-file total summary OK\n")
    return True


def check_multi_file_failure() -> bool:
    """
    Check fail-fast behavior when a file is invalid.
    """
    print("=== Testing multi-file failure ===")
    valid_file = RUN_CONFIG.test_dir / "test.ll"
    missing_file = RUN_CONFIG.test_dir / "does-not-exist.ll"

    result = run_analyzer([str(valid_file), str(missing_file)])
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode == 0:
        print("  ❌ expected non-zero exit code")
        print(output)
        print()
        return False

    if str(missing_file) not in output:
        print("  ❌ missing filename not mentioned in output")
        print(output)
        print()
        return False

    print("  ✅ multi-file failure OK\n")
    return True


def check_cli_parsing_and_filters() -> bool:
    """
    Check CLI parsing (errors) + main filters.
    """
    print("=== Testing CLI parsing & filters ===")
    ok = True

    sample = RUN_CONFIG.test_dir / "false-positif/unique_ptr_state.cpp"
    sample_warning = RUN_CONFIG.test_dir / "uninitialized-variable/uninitialized-local-basic.c"
    sample_c = RUN_CONFIG.test_dir / "alloca/oversized-constant.c"
    resource_model = Path("models/resource-lifetime/generic.txt")
    escape_model = Path("models/stack-escape/generic.txt")
    buffer_model = Path("models/buffer-overflow/generic.txt")

    def run_success_case(label: str, args: list[str], required: Optional[list[str]] = None, fmt: str = "text") -> bool:
        result = run_analyzer(args)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ {label} failed (code {result.returncode})")
            print(output)
            return False

        required = required or []
        if fmt == "json":
            try:
                payload = json.loads(result.stdout or "")
            except json.JSONDecodeError as exc:
                print(f"  ❌ {label} produced invalid JSON: {exc}")
                print(result.stdout or "")
                return False
            if not isinstance(payload, dict):
                print(f"  ❌ {label} JSON root is not an object")
                print(result.stdout or "")
                return False
        elif fmt == "sarif":
            try:
                payload = json.loads(result.stdout or "")
            except json.JSONDecodeError as exc:
                print(f"  ❌ {label} produced invalid SARIF JSON: {exc}")
                print(result.stdout or "")
                return False
            if payload.get("version") != "2.1.0":
                print(f"  ❌ {label} produced unexpected SARIF version")
                print(result.stdout or "")
                return False

        for needle in required:
            if needle not in output:
                print(f"  ❌ {label} missing expected output: {needle}")
                print(output)
                return False

        print(f"  ✅ {label} OK")
        return True

    # Missing-argument cases (all options requiring a value).
    missing_arg_cases = [
        ("--only-file", "Missing argument for --only-file"),
        ("--only-dir", "Missing argument for --only-dir"),
        ("--exclude-dir", "Missing argument for --exclude-dir"),
        ("--only-func", "Missing argument for --only-func"),
        ("--only-function", "Missing argument for --only-function"),
        ("--stack-limit", "Missing argument for --stack-limit"),
        ("--dump-ir", "Missing argument for --dump-ir"),
        ("--compile-arg", "Missing argument for --compile-arg"),
        ("--analysis-profile", "Missing argument for --analysis-profile"),
        ("--jobs", "Missing argument for --jobs"),
        ("--resource-model", "Missing argument for --resource-model"),
        ("--escape-model", "Missing argument for --escape-model"),
        ("--buffer-model", "Missing argument for --buffer-model"),
        ("--resource-summary-cache-dir", "Missing argument for --resource-summary-cache-dir"),
        ("--compile-commands", "Missing argument for --compile-commands"),
        ("--compdb", "Missing argument for --compdb"),
        ("--base-dir", "Missing argument for --base-dir"),
        ("-I", "Missing argument for -I"),
        ("-D", "Missing argument for -D"),
    ]
    for flag, needle in missing_arg_cases:
        result = run_analyzer([flag])
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode == 0 or needle not in output:
            print(f"  ❌ {flag} missing-arg handling")
            print(output)
            ok = False
        else:
            print(f"  ✅ {flag} missing-arg OK")

    # Unknown option and invalid values.
    result = run_analyzer(["--unknown-option"])
    output = (result.stdout or "") + (result.stderr or "")
    if "Unknown option: --unknown-option" not in output:
        print("  ❌ unknown option handling")
        print(output)
        ok = False
    elif "Did you mean" in output:
        print("  ❌ unknown option unexpectedly suggested a flag")
        print(output)
        ok = False
    else:
        print("  ✅ unknown option OK")

    unknown_suggestion_cases = [
        ("--only-fil", "Did you mean '--only-file'?"),
        ("--format=sraif", "Did you mean '--format=sarif'?"),
        ("--mdoe=abi", "Did you mean '--mode=abi'?"),
    ]
    for bad_opt, expected_hint in unknown_suggestion_cases:
        result = run_analyzer([bad_opt])
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode == 0 or expected_hint not in output:
            print(f"  ❌ suggestion handling failed: {bad_opt}")
            print(output)
            ok = False
        else:
            print(f"  ✅ suggestion handling OK: {bad_opt}")

    invalid_value_cases = [
        (["--jobs=0", str(sample)], "Invalid --jobs value:"),
        (["--jobs=x", str(sample)], "Invalid --jobs value:"),
        (["--jobs=-1", str(sample)], "Invalid --jobs value:"),
        (["--analysis-profile=unknown", str(sample)], "Invalid --analysis-profile value:"),
        (["--stack-limit=oops", str(sample)], "Invalid --stack-limit value:"),
        (["--mode=unknown", str(sample)], "Unknown mode: unknown (expected 'ir' or 'abi')"),
    ]
    for args, needle in invalid_value_cases:
        result = run_analyzer(args)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode == 0 or needle not in output:
            print(f"  ❌ invalid-value handling failed: {' '.join(args)}")
            print(output)
            ok = False
        else:
            print(f"  ✅ invalid-value handling OK: {' '.join(args)}")

    with tempfile.TemporaryDirectory(prefix="ct_cli_option_matrix_") as tmp:
        tmpdir = Path(tmp)
        dump_ir_space = tmpdir / "dump-space.ll"
        dump_ir_eq = tmpdir / "dump-eq.ll"
        resource_cache = tmpdir / "resource-cache"
        compdb = tmpdir / "compile_commands.json"

        entries = [
            {
                "directory": str(sample.resolve().parent),
                "file": str(sample.resolve()),
                "arguments": ["clang", "-c", str(sample.resolve())],
            }
        ]
        compdb.write_text(json.dumps(entries), encoding="utf-8")

        success_cases = [
            ("--demangle", [str(sample), "--demangle", "--only-function=transition"], ["Function:"], "text"),
            ("--quiet", [str(sample), "--quiet"], [], "text"),
            ("--verbose", [str(sample), "--verbose", "--only-function=transition"], ["Function:"], "text"),
            ("--STL", [str(sample), "--STL", "--only-function=transition"], ["Function:"], "text"),
            ("--stl", [str(sample), "--stl", "--only-function=transition"], ["Function:"], "text"),
            ("--only-file space", [str(sample), "--only-file", str(sample), "--only-function=transition"], ["Function:"], "text"),
            ("--only-file equals", [str(sample), f"--only-file={sample}", "--only-function=transition"], ["Function:"], "text"),
            ("--only-dir space", [str(sample), "--only-dir", str(sample.parent), "--only-function=transition"], ["Function:"], "text"),
            ("--only-dir equals", [str(sample), f"--only-dir={sample.parent}", "--only-function=transition"], ["Function:"], "text"),
            ("--exclude-dir space", [str(sample), "--exclude-dir", "never-match-dir", "--only-function=transition"], ["Function:"], "text"),
            ("--exclude-dir equals", [str(sample), "--exclude-dir=never-match-dir", "--only-function=transition"], ["Function:"], "text"),
            ("--only-function equals", [str(sample), "--only-function=transition"], ["Function:"], "text"),
            ("--only-function space", [str(sample), "--only-function", "transition"], ["Function:"], "text"),
            ("--only-func equals", [str(sample), "--only-func=transition"], ["Function:"], "text"),
            ("--only-func space", [str(sample), "--only-func", "transition"], ["Function:"], "text"),
            ("--stack-limit space", [str(sample_c), "--stack-limit", "8MiB"], ["Function:"], "text"),
            ("--stack-limit equals", [str(sample_c), "--stack-limit=8MiB"], ["Function:"], "text"),
            ("--dump-filter", [str(sample), "--dump-filter", "--only-function=transition"], ["Function:"], "text"),
            ("--dump-ir space", [str(sample_c), "--dump-ir", str(dump_ir_space)], ["Function:"], "text"),
            ("--dump-ir equals", [str(sample_c), f"--dump-ir={dump_ir_eq}"], ["Function:"], "text"),
            ("-I<dir>", [str(sample), f"-I{sample.parent}", "--only-function=transition"], ["Function:"], "text"),
            ("-I <dir>", [str(sample), "-I", str(sample.parent), "--only-function=transition"], ["Function:"], "text"),
            ("-D<name>", [str(sample), "-DHELLO", "--only-function=transition"], ["Function:"], "text"),
            ("-D <name>", [str(sample), "-D", "HELLO", "--only-function=transition"], ["Function:"], "text"),
            ("--compile-arg", [str(sample), "--compile-arg=-I.", "--only-function=transition"], ["Function:"], "text"),
            ("--compdb-fast", [str(sample), "--compdb-fast", "--only-function=transition"], ["Function:"], "text"),
            ("--analysis-profile space", [str(sample), "--analysis-profile", "fast", "--only-function=transition"], ["Function:"], "text"),
            ("--analysis-profile equals", [str(sample), "--analysis-profile=full", "--only-function=transition"], ["Function:"], "text"),
            ("--jobs space", [str(sample), "--jobs", "2", "--only-function=transition"], ["Function:"], "text"),
            ("--jobs equals", [str(sample), "--jobs=2", "--only-function=transition"], ["Function:"], "text"),
            ("--timing", [str(sample), "--timing", "--only-function=transition"], ["Function:"], "text"),
            ("--resource-model space", [str(sample), "--resource-model", str(resource_model), "--only-function=transition"], ["Function:"], "text"),
            ("--resource-model equals", [str(sample), f"--resource-model={resource_model}", "--only-function=transition"], ["Function:"], "text"),
            ("--escape-model space", [str(sample), "--escape-model", str(escape_model), "--only-function=transition"], ["Function:"], "text"),
            ("--escape-model equals", [str(sample), f"--escape-model={escape_model}", "--only-function=transition"], ["Function:"], "text"),
            ("--buffer-model space", [str(sample), "--buffer-model", str(buffer_model), "--only-function=transition"], ["Function:"], "text"),
            ("--buffer-model equals", [str(sample), f"--buffer-model={buffer_model}", "--only-function=transition"], ["Function:"], "text"),
            ("--resource-cross-tu", [str(sample), "--resource-cross-tu", "--only-function=transition"], ["Function:"], "text"),
            ("--no-resource-cross-tu", [str(sample), "--no-resource-cross-tu", "--only-function=transition"], ["Function:"], "text"),
            ("--uninitialized-cross-tu", [str(sample), "--uninitialized-cross-tu", "--only-function=transition"], ["Function:"], "text"),
            ("--no-uninitialized-cross-tu", [str(sample), "--no-uninitialized-cross-tu", "--only-function=transition"], ["Function:"], "text"),
            ("--resource-summary-cache-dir space", [str(sample), "--resource-summary-cache-dir", str(resource_cache), "--only-function=transition"], ["Function:"], "text"),
            ("--resource-summary-cache-dir equals", [str(sample), f"--resource-summary-cache-dir={resource_cache}", "--only-function=transition"], ["Function:"], "text"),
            ("--resource-summary-cache-memory-only", [str(sample), "--resource-summary-cache-memory-only", "--only-function=transition"], ["Function:"], "text"),
            (
                "--warnings-only",
                [str(sample_warning), "--warnings-only"],
                ["Function: read_uninitialized_basic"],
                "text",
            ),
            ("--format=json", [str(sample), "--format=json"], [], "json"),
            ("--format=sarif", [str(sample), "--format=sarif"], [], "sarif"),
            ("--format=human", [str(sample), "--format=human", "--only-function=transition"], ["Function:"], "text"),
            ("--base-dir space", [str(sample), "--format=sarif", "--base-dir", str(sample.parent)], [], "sarif"),
            ("--base-dir equals", [str(sample), "--format=sarif", f"--base-dir={sample.parent}"], [], "sarif"),
            ("--mode=ir", [str(sample), "--mode=ir", "--only-function=transition"], ["Function:"], "text"),
            ("--mode=abi", [str(sample), "--mode=abi", "--only-function=transition"], ["Function:"], "text"),
            ("--compile-commands space", [str(sample), "--compile-commands", str(compdb), "--only-function=transition"], ["Function:"], "text"),
            ("--compile-commands equals", [str(sample), f"--compile-commands={compdb}", "--only-function=transition"], ["Function:"], "text"),
            ("--compdb space", [str(sample), "--compdb", str(compdb), "--only-function=transition"], ["Function:"], "text"),
            ("--compdb equals", [str(sample), f"--compdb={compdb}", "--only-function=transition"], ["Function:"], "text"),
            ("--include-compdb-deps", [f"--compile-commands={compdb}", "--include-compdb-deps", "--warnings-only"], [], "text"),
        ]

        for label, args, required, fmt in success_cases:
            if not run_success_case(label, args, required, fmt):
                ok = False

        if not dump_ir_space.exists():
            print(f"  ❌ --dump-ir space did not create output file: {dump_ir_space}")
            ok = False
        else:
            print("  ✅ --dump-ir space created file")
        if not dump_ir_eq.exists():
            print(f"  ❌ --dump-ir equals did not create output file: {dump_ir_eq}")
            ok = False
        else:
            print("  ✅ --dump-ir equals created file")

    print()
    return ok


def check_only_func_uninitialized() -> bool:
    """
    Regression: --only-func must keep interprocedural uninitialized warnings.
    """
    print("=== Testing --only-func for uninitialized analysis ===")
    sample = RUN_CONFIG.test_dir / "uninitialized-variable/uninitialized-local-interproc-struct-partial.cpp"
    result = run_analyzer([str(sample), "--only-func=main", "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")

    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    must_contain = [
        "Function: main",
        "potential read of uninitialized local variable 'cfg'",
    ]
    for needle in must_contain:
        if needle not in output:
            print(f"  ❌ missing expected output: {needle}")
            print(output)
            print()
            return False

    print("  ✅ --only-func preserves warning\n")
    return True


def check_warnings_only_filters_function_listing() -> bool:
    """
    Regression: --warnings-only must only list functions that carry warnings/errors.
    """
    print("=== Testing --warnings-only function listing filter ===")
    sample = (
        RUN_CONFIG.test_dir
        / "uninitialized-variable/uninitialized-local-warnings-only-function-filter.c"
    )
    result = run_analyzer([str(sample), "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")

    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    required = [
        "Function: read_uninitialized_value",
        "potential read of uninitialized local variable 'value'",
    ]
    forbidden = [
        "Function: clean_value",
        "Function: main",
    ]

    for needle in required:
        if needle not in output:
            print(f"  ❌ missing expected output: {needle}")
            print(output)
            print()
            return False

    for needle in forbidden:
        if needle in output:
            print(f"  ❌ unexpected function listed in --warnings-only output: {needle}")
            print(output)
            print()
            return False

    print("  ✅ --warnings-only function listing filter OK\n")
    return True


def check_uninitialized_verbose_ctor_trace() -> bool:
    """
    Regression: --verbose must expose whether default-constructor evidence was
    detected (at constructor mark time and/or never-init triage).
    """
    print("=== Testing verbose constructor detection trace ===")
    cases = [
        (
            "default ctor detected",
            RUN_CONFIG.test_dir / "uninitialized-variable/uninitialized-local-opaque-ctor.cpp",
            [
                "[uninit][ctor]",
                "local=obj",
                "default_ctor_detected=yes",
                "action=mark_default_ctor",
            ],
        ),
        (
            "default ctor not detected",
            RUN_CONFIG.test_dir / "uninitialized-variable/uninitialized-local-cpp-trivial-ctor.cpp",
            [
                "[uninit][ctor]",
                "local=app",
                "default_ctor_detected=no",
                "action=suppress_never_initialized",
            ],
        ),
    ]

    for label, fixture, needles in cases:
        result = run_analyzer([str(fixture), "--verbose", "--warnings-only"])
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ {label} failed (code {result.returncode})")
            print(output)
            print()
            return False
        for needle in needles:
            if needle not in output:
                print(f"  ❌ {label}: missing expected verbose trace token: {needle}")
                print(output)
                print()
                return False

    print("  ✅ verbose ctor trace OK\n")
    return True


def check_uninitialized_unsummarized_defined_bool_out_param() -> bool:
    """
    Regression: defined-but-unsummarized bool/status calls guarded by return-value
    control flow must mark out-param writes (self-analysis case).
    """
    print("=== Testing unsummarized defined bool out-param fallback ===")
    sample = Path("src/analysis/SizeMinusKWrites.cpp")
    compdb = Path("build/compile_commands.json")

    if not sample.exists():
        print(f"  [info] sample not found, skipping: {sample}\n")
        return True
    if not compdb.exists():
        print(f"  [info] compile_commands not found, skipping: {compdb}\n")
        return True

    result = run_analyzer(
        [str(sample), f"--compile-commands={compdb}", "--warnings-only"]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    forbidden = "potential read of uninitialized local variable 'lf'"
    if forbidden in output:
        print(f"  ❌ unexpected warning still present: {forbidden}")
        print(output)
        print()
        return False

    print("  ✅ unsummarized defined bool out-param fallback OK\n")
    return True


def check_uninitialized_optional_receiver_index_repro() -> bool:
    """
    Reproducer: optional receiver-index tracking passed by value can trigger
    a false positive on local initialization.

    Regression target: this warning must stay suppressed across toolchains.
    """
    print("=== Testing optional receiver index false-positive reproducer ===")
    sample = (
        RUN_CONFIG.test_dir
        / "uninitialized-variable/uninitialized-local-cpp-optional-receiver-index.cpp"
    )

    if not sample.exists():
        print(f"  [info] sample not found, skipping: {sample}\n")
        return True

    result = run_analyzer([str(sample), "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    expected = "potential read of uninitialized local variable 'methodReceiverIdx'"
    if expected in output:
        print(f"  ❌ unexpected warning still present: {expected}")
        print(output)
        print()
        return False

    print("  ✅ optional receiver index false-positive suppressed\n")
    return True


def check_unknown_alloca_virtual_callback_escape() -> bool:
    """
    Regression: unknown-origin unnamed allocas must not be silently treated as
    compiler temporaries in virtual-like indirect callback paths.
    """
    print("=== Testing unknown alloca virtual callback escape ===")
    sample = RUN_CONFIG.test_dir / "escape-stack/virtual-unnamed-alloca-unknown-target.ll"
    result = run_analyzer([str(sample)])
    output = (result.stdout or "") + (result.stderr or "")
    norm_output = normalize(output)

    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    must_contain = [
        "Function: test_virtual_unknown",
        "stack pointer escape: address of variable '<unnamed>' escapes this function",
    ]
    for needle in must_contain:
        if normalize(needle) not in norm_output:
            print(f"  ❌ missing expected output: {needle}")
            print(output)
            print()
            return False

    print("  ✅ unknown-origin unnamed alloca still reports escape\n")
    return True


def check_resource_lifetime_cross_tu() -> bool:
    """
    Regression: cross-TU resource summaries must propagate acquire/release effects
    across separate translation units.
    """
    print("=== Testing resource lifetime cross-TU summaries ===")
    model = "models/resource-lifetime/generic.txt"

    wrapper_use = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-wrapper-use.c"
    result = run_analyzer([str(wrapper_use), f"--resource-model={model}", "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "single-file wrapper run failed"):
        return False
    if not expect_contains(
        output,
        "Resource inter-procedural analysis: unavailable",
        "missing inter-proc unavailable status message in single-file mode",
    ):
        return False
    if not expect_contains(
        output,
        "inter-procedural resource analysis incomplete: handle 'h'",
        "missing IncompleteInterproc warning in single-file wrapper case",
    ):
        return False
    if not expect_not_contains(
        output,
        "potential double release: 'GenericHandle' handle 'h'",
        "unexpected double release in single-file wrapper case",
    ):
        return False

    wrapper_def = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-wrapper-def.c"
    result = run_analyzer(
        [
            str(wrapper_def),
            str(wrapper_use),
            f"--resource-model={model}",
            "--jobs=2",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "wrapper cross-TU run failed"):
        return False
    if not expect_contains(
        output,
        "Resource inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing inter-proc enabled status message in cross-TU mode",
    ):
        return False
    if not expect_contains(output, "jobs: 2", "missing jobs count in inter-proc enabled status message"):
        return False
    if not expect_not_contains(
        output,
        "potential double release: 'GenericHandle' handle 'h'",
        "unexpected double release in cross-TU wrapper release case",
    ):
        return False

    wrapper_leak = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-wrapper-leak-use.c"
    result = run_analyzer(
        [str(wrapper_def), str(wrapper_leak), f"--resource-model={model}", "--warnings-only"]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "wrapper leak cross-TU run failed"):
        return False
    if not expect_contains(
        output,
        "potential resource leak: 'GenericHandle' acquired in handle 'h'",
        "missing leak warning in cross-TU wrapper leak case",
    ):
        return False

    ret_def = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-return-def.c"
    ret_use = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-return-use.c"
    result = run_analyzer(
        [str(ret_def), str(ret_use), f"--resource-model={model}", "--warnings-only"]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "return cross-TU run failed"):
        return False
    if not expect_not_contains(
        output,
        "potential double release: 'HeapAlloc' handle 'p'",
        "unexpected double release in cross-TU acquire_ret case",
    ):
        return False

    result = run_analyzer(
        [
            str(ret_def),
            str(ret_use),
            f"--resource-model={model}",
            "--no-resource-cross-tu",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "return cross-TU disabled run failed"):
        return False
    if not expect_contains(
        output,
        "inter-procedural resource analysis incomplete: handle 'p'",
        "expected local-only incomplete inter-proc warning is missing with --no-resource-cross-tu",
    ):
        return False

    cache_dir = Path(".cache/run_test_resource_summary")
    if cache_dir.exists():
        shutil.rmtree(cache_dir, ignore_errors=True)
    result = run_analyzer_uncached(
        [
            str(ret_def),
            str(ret_use),
            f"--resource-model={model}",
            f"--resource-summary-cache-dir={cache_dir}",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "return cross-TU cache run failed"):
        return False
    if not list(cache_dir.glob("*.json")):
        return fail_check("cross-TU cache directory was not populated", output)

    memory_only_cache_dir = Path(".cache/run_test_resource_summary_memory_only")
    if memory_only_cache_dir.exists():
        shutil.rmtree(memory_only_cache_dir, ignore_errors=True)
    result = run_analyzer_uncached(
        [
            str(ret_def),
            str(ret_use),
            f"--resource-model={model}",
            "--resource-summary-cache-memory-only",
            f"--resource-summary-cache-dir={memory_only_cache_dir}",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "return cross-TU memory-only cache run failed"):
        return False
    if not expect_contains(output, "cache: memory-only", "missing memory-only cache status message"):
        return False
    if list(memory_only_cache_dir.glob("*.json")):
        return fail_check("memory-only cache mode unexpectedly wrote summary files", output)

    print("  ✅ cross-TU resource summaries OK\n")
    return True


def check_uninitialized_cross_tu() -> bool:
    """
    Regression: cross-TU uninitialized summaries must propagate indirect out-param
    writes across separate translation units.
    """
    print("=== Testing uninitialized cross-TU summaries ===")

    wrapper_def = RUN_CONFIG.test_dir / "uninitialized-variable/cross-tu-uninitialized-wrapper-def.c"
    wrapper_use = RUN_CONFIG.test_dir / "uninitialized-variable/cross-tu-uninitialized-wrapper-use.c"

    result = run_analyzer([str(wrapper_use), "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "single-file uninitialized run failed"):
        return False
    if not expect_contains(
        output,
        "potential read of uninitialized local variable 'value'",
        "missing uninitialized warning in single-file wrapper case",
    ):
        return False

    result = run_analyzer([str(wrapper_def), str(wrapper_use), "--jobs=2", "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "cross-TU uninitialized run failed"):
        return False
    if not expect_contains(
        output,
        "Uninitialized inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing uninitialized cross-TU enabled status message",
    ):
        return False
    if not expect_not_contains(
        output,
        "potential read of uninitialized local variable 'value'",
        "unexpected uninitialized warning in cross-TU wrapper case",
    ):
        return False

    sret_def = RUN_CONFIG.test_dir / "uninitialized-variable/cross-tu-uninitialized-sret-status-def.cpp"
    sret_use = RUN_CONFIG.test_dir / "uninitialized-variable/cross-tu-uninitialized-sret-status-use.cpp"
    result = run_analyzer([str(sret_def), str(sret_use), "--jobs=2", "--warnings-only"])
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "cross-TU sret-status run failed"):
        return False
    if not expect_not_contains(
        output,
        "potential read of uninitialized local variable 'out'",
        "unexpected uninitialized warning in cross-TU sret-status wrapper case",
    ):
        return False

    result = run_analyzer(
        [
            str(wrapper_def),
            str(wrapper_use),
            "--no-uninitialized-cross-tu",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "cross-TU disabled uninitialized run failed"):
        return False
    if not expect_contains(
        output,
        "potential read of uninitialized local variable 'value'",
        "expected local-only uninitialized warning is missing with --no-uninitialized-cross-tu",
    ):
        return False

    print("  ✅ cross-TU uninitialized summaries OK\n")
    return True


def check_null_deref_nested_inter_tu() -> bool:
    """
    Regression: nested null-deref cases must still be reported when the analyzer
    runs in multi-file mode with inter-TU summaries enabled.
    """
    print("=== Testing null deref nested cases in inter-TU mode ===")

    nested_fixture = fixture_path_with_fallback(
        "security/null-dereference/16_null_deref_nested.c",
        "files/16_null_deref_nested.c",
    )
    helper_fixture = RUN_CONFIG.test_dir / "test-multi-tu/worker.c"
    if not nested_fixture.exists() or not helper_fixture.exists():
        print("  ❌ missing null-deref inter-TU fixture files")
        print(f"     expected: {nested_fixture} and {helper_fixture}")
        print()
        return False

    result = run_analyzer(
        [
            str(nested_fixture),
            str(helper_fixture),
            "--jobs=2",
            "--resource-cross-tu",
            "--uninitialized-cross-tu",
            "--resource-model=models/resource-lifetime/generic.txt",
            "--escape-model=models/stack-escape/generic.txt",
            "--buffer-model=models/buffer-overflow/generic.txt",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")

    if not expect_returncode_zero(result, output, "null-deref inter-TU run failed"):
        return False
    if not expect_contains(
        output,
        "Resource inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing resource cross-TU enabled status for null-deref inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Uninitialized inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing uninitialized cross-TU enabled status for null-deref inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: vuln_nested_if_unchecked_malloc",
        "missing nested-if unchecked allocator function in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: vuln_nested_loop_unchecked_malloc",
        "missing nested-loop unchecked allocator function in inter-TU run",
    ):
        return False
    if output.count(
        "pointer comes from allocator return value and is dereferenced without a provable null-check"
    ) < 2:
        return fail_check(
            "missing one unchecked-allocator null-deref warning in nested inter-TU run", output
        )
    if not expect_contains(
        output,
        "Function: vuln_nested_if_null_branch",
        "missing nested-if null-branch function in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "control flow proves pointer is null on this branch before dereference",
        "missing null-branch dereference diagnostic in inter-TU run",
    ):
        return False

    print("  ✅ nested null-deref diagnostics OK in inter-TU mode\n")
    return True


def check_integer_overflow_advanced_inter_tu() -> bool:
    """
    Regression: advanced integer-overflow diagnostics must remain detectable in
    multi-file runs with inter-TU mode enabled.
    """
    print("=== Testing advanced integer overflow cases in inter-TU mode ===")

    def_file = RUN_CONFIG.test_dir / "integer-overflow/cross-tu-tricky-def.c"
    use_file = RUN_CONFIG.test_dir / "integer-overflow/cross-tu-tricky-use.c"
    if not def_file.exists() or not use_file.exists():
        print("  ❌ missing integer-overflow inter-TU fixture files")
        print(f"     expected: {def_file} and {use_file}")
        print()
        return False

    result = run_analyzer(
        [
            str(def_file),
            str(use_file),
            "--jobs=2",
            "--analysis-profile=full",
            "--resource-cross-tu",
            "--uninitialized-cross-tu",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")

    if not expect_returncode_zero(result, output, "integer-overflow inter-TU run failed"):
        return False
    if not expect_contains(
        output,
        "Uninitialized inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing inter-TU enabled status in integer-overflow inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: io_cross_signed_overflow",
        "missing cross-TU signed-overflow function in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "potential signed integer overflow in arithmetic operation",
        "missing signed-overflow arithmetic diagnostic in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: io_cross_truncation_alloc",
        "missing cross-TU truncation function in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "potential integer truncation in size computation before 'malloc'",
        "missing truncation-before-malloc diagnostic in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: io_cross_signed_to_size_copy",
        "missing cross-TU signed-to-size function in inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "potential signed-to-size conversion before 'memcpy'",
        "missing signed-to-size conversion diagnostic in inter-TU run",
    ):
        return False

    print("  ✅ advanced integer-overflow diagnostics OK in inter-TU mode\n")
    return True


def check_use_after_free_advanced_inter_tu() -> bool:
    """
    Regression: nested use-after-free and double-release cases must be detected
    when release effects come from cross-TU summaries.
    """
    print("=== Testing use-after-free nested cases in inter-TU mode ===")

    def_file = RUN_CONFIG.test_dir / "use-after-free/cross-tu-uaf-def.c"
    use_file = RUN_CONFIG.test_dir / "use-after-free/cross-tu-uaf-use.c"
    if not def_file.exists() or not use_file.exists():
        print("  ❌ missing use-after-free inter-TU fixture files")
        print(f"     expected: {def_file} and {use_file}")
        print()
        return False

    model = "models/resource-lifetime/generic.txt"
    result = run_analyzer(
        [
            str(def_file),
            str(use_file),
            "--jobs=2",
            "--analysis-profile=full",
            "--resource-cross-tu",
            f"--resource-model={model}",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")

    if not expect_returncode_zero(result, output, "use-after-free inter-TU run failed"):
        return False
    if not expect_contains(
        output,
        "Resource inter-procedural analysis: enabled (cross-TU summaries across 2 files",
        "missing resource cross-TU enabled status in use-after-free inter-TU run",
    ):
        return False
    if not expect_contains(
        output,
        "Function: io_cross_uaf_nested_if",
        "missing nested-if cross-TU UAF function in output",
    ):
        return False
    if not expect_contains(
        output,
        "potential use-after-release: 'GenericHandle' handle 'h'",
        "missing cross-TU use-after-release diagnostic",
    ):
        return False
    if not expect_contains(
        output,
        "Function: io_cross_double_release_nested_loop",
        "missing nested-loop cross-TU double-release function in output",
    ):
        return False
    if not expect_contains(
        output,
        "potential double release: 'GenericHandle' handle 'h'",
        "missing cross-TU double-release diagnostic",
    ):
        return False
    if not expect_not_contains(
        output,
        "inter-procedural resource analysis incomplete: handle 'h'",
        "unexpected IncompleteInterproc warning in cross-TU enabled run",
    ):
        return False

    result = run_analyzer(
        [
            str(def_file),
            str(use_file),
            "--jobs=2",
            "--analysis-profile=full",
            "--no-resource-cross-tu",
            f"--resource-model={model}",
            "--warnings-only",
        ]
    )
    output = (result.stdout or "") + (result.stderr or "")
    if not expect_returncode_zero(result, output, "use-after-free cross-TU disabled run failed"):
        return False
    if not expect_not_contains(
        output,
        "potential use-after-release: 'GenericHandle' handle 'h'",
        "unexpected cross-TU use-after-release diagnostic with --no-resource-cross-tu",
    ):
        return False
    if not expect_not_contains(
        output,
        "potential double release: 'GenericHandle' handle 'h'",
        "unexpected cross-TU double-release diagnostic with --no-resource-cross-tu",
    ):
        return False

    print("  ✅ nested use-after-free diagnostics OK in inter-TU mode\n")
    return True


def check_escape_model_rejects_unsupported_brackets() -> bool:
    """
    Regression: stack escape model must reject unsupported [..] classes
    with an explicit error instead of silently mis-matching patterns.
    """
    print("=== Testing stack escape model rejects unsupported bracket classes ===")
    with tempfile.TemporaryDirectory(prefix="ct_escape_model_brackets_") as tmp:
        tmpdir = Path(tmp)
        source_file = tmpdir / "sample.c"
        model_file = tmpdir / "invalid-model.txt"
        source_file.write_text("int main(void) { return 0; }\n", encoding="utf-8")
        model_file.write_text("noescape_arg vkFoo[AB] 0\n", encoding="utf-8")

        result = run_analyzer([str(source_file), f"--escape-model={model_file}"])
        output = (result.stdout or "") + (result.stderr or "")
        if not expect_returncode_zero(result, output, "escape-model validation run failed"):
            return False
        if not expect_contains(
            output,
            "stack escape model ignored: unsupported character class syntax '[...]'",
            "missing explicit unsupported bracket-class warning in stack escape model",
        ):
            return False

    print("  ✅ stack escape model bracket-class rejection OK\n")
    return True


def check_docker_entrypoint_guardrails() -> bool:
    """
    Regression: docker wrapper should only create compatibility symlinks under
    allowlisted roots and should fail cleanly when analyzer binary is missing.
    """
    print("=== Testing docker entrypoint guardrails ===")
    module, error = load_docker_entrypoint_module()
    if module is None:
        return fail_check(error)

    original_allowlist = os.environ.get("CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS")
    try:
        with tempfile.TemporaryDirectory(prefix="ct_entrypoint_ws_") as tmp:
            workspace = Path(tmp)
            module.WORKSPACE = str(workspace)
            build_dir = workspace / "build"
            build_dir.mkdir(parents=True, exist_ok=True)
            compdb_path = build_dir / "compile_commands.json"

            blocked_root = Path("/malicious-coretrace-compat-root")
            blocked_entry_dir = blocked_root / "build"
            compdb_path.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(blocked_entry_dir),
                            "file": str(blocked_root / "source.c"),
                            "arguments": ["clang", "-c", "source.c"],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            os.environ["CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS"] = "/tmp:/var/tmp"
            blocked_log = io.StringIO()
            with contextlib.redirect_stderr(blocked_log):
                module.ensure_compdb_compat_symlink(str(compdb_path))
            if "outside allowlist roots" not in blocked_log.getvalue():
                return fail_check("missing allowlist refusal message for blocked symlink root")
            if blocked_root.is_symlink():
                return fail_check("blocked compatibility symlink was unexpectedly created")

            allowed_root = Path(f"/tmp/ct_entrypoint_link_{uuid.uuid4().hex[:10]}")
            allowed_entry_dir = allowed_root / "build"
            compdb_path.write_text(
                json.dumps(
                    [
                        {
                            "directory": str(allowed_entry_dir),
                            "file": str(allowed_root / "source.c"),
                            "arguments": ["clang", "-c", "source.c"],
                        }
                    ]
                ),
                encoding="utf-8",
            )
            if allowed_root.exists() or allowed_root.is_symlink():
                try:
                    if allowed_root.is_symlink():
                        allowed_root.unlink()
                    elif allowed_root.is_dir():
                        shutil.rmtree(allowed_root, ignore_errors=True)
                    else:
                        allowed_root.unlink()
                except OSError:
                    pass
            allowed_log = io.StringIO()
            with contextlib.redirect_stderr(allowed_log):
                module.ensure_compdb_compat_symlink(str(compdb_path))
            if not allowed_root.is_symlink():
                return fail_check("allowlisted compatibility symlink was not created")
            if allowed_root.resolve() != workspace.resolve():
                return fail_check("allowlisted compatibility symlink target is incorrect")
            allowed_root.unlink(missing_ok=True)

            exec_log = io.StringIO()
            with contextlib.redirect_stderr(exec_log):
                ret = module.exec_analyzer(["/definitely/missing/coretrace-analyzer-bin"])
            if ret != 127:
                return fail_check(f"expected exec_analyzer missing-binary exit code 127, got {ret}")
            if "analyzer executable not found" not in exec_log.getvalue():
                return fail_check("missing user-friendly execvp error message")
    finally:
        if original_allowlist is None:
            os.environ.pop("CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS", None)
        else:
            os.environ["CORETRACE_COMPAT_SYMLINK_ALLOWED_ROOTS"] = original_allowlist

    print("  ✅ docker entrypoint guardrails OK\n")
    return True


def check_compdb_as_default_input_source() -> bool:
    """
    Regression: when no positional inputs are provided and --compile-commands is set,
    analyzer must use compile_commands.json entries as input source-of-truth.
    """
    print("=== Testing compile_commands as default input source ===")
    with tempfile.TemporaryDirectory(prefix="ct_compdb_default_") as tmp:
        tmpdir = Path(tmp)
        c_file = tmpdir / "from_compdb.c"
        empty_cpp = tmpdir / "empty_tu.cpp"
        objc_file = tmpdir / "ignored.m"
        compdb = tmpdir / "compile_commands.json"

        c_file.write_text("int from_compdb(void) { return 42; }\n", encoding="utf-8")
        empty_cpp.write_text("namespace only_decl {}\n", encoding="utf-8")
        objc_file.write_text("int ignored_objc(void) { return 0; }\n", encoding="utf-8")

        entries = [
            {
                "directory": str(tmpdir),
                "file": str(c_file),
                "arguments": ["clang", "-c", str(c_file)],
            },
            {
                "directory": str(tmpdir),
                "file": str(empty_cpp),
                "arguments": ["clang++", "-c", str(empty_cpp)],
            },
            {
                "directory": str(tmpdir),
                "file": str(objc_file),
                "arguments": ["clang", "-c", str(objc_file)],
            },
        ]
        compdb.write_text(json.dumps(entries), encoding="utf-8")

        result = run_analyzer([f"--compile-commands={compdb}"])
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ default compdb input run failed (code {result.returncode})")
            print(output)
            print()
            return False

        if "No explicit input files provided: using 2 supported file(s) from compile_commands.json" not in output:
            print("  ❌ missing compdb default input status message")
            print(output)
            print()
            return False
        if "skipped 1 unsupported entry/entries" not in output:
            print("  ❌ missing unsupported entry count in compdb default input status")
            print(output)
            print()
            return False
        if "Function: from_compdb" not in output:
            print("  ❌ expected function from compdb-driven input is missing")
            print(output)
            print()
            return False
        if "No analyzable functions in:" not in output or "empty_tu.cpp (skipping)" not in output:
            print("  ❌ missing informational skip message for empty translation unit")
            print(output)
            print()
            return False
        if "Unsupported input file type:" in output:
            print("  ❌ analyzer still attempted unsupported compdb entry")
            print(output)
            print()
            return False

    print("  ✅ compile_commands default input source OK\n")
    return True


def check_exclude_dir_filter() -> bool:
    """
    Regression: --exclude-dir must filter input files before analysis.
    """
    print("=== Testing --exclude-dir input filtering ===")
    with tempfile.TemporaryDirectory(prefix="ct_exclude_dir_") as tmp:
        tmpdir = Path(tmp)
        keep_dir = tmpdir / "keep"
        skip_dir = tmpdir / "skip/sub"
        keep_dir.mkdir(parents=True, exist_ok=True)
        skip_dir.mkdir(parents=True, exist_ok=True)

        keep_file = keep_dir / "keep.c"
        skip_file = skip_dir / "skip.c"
        compdb = tmpdir / "compile_commands.json"

        keep_file.write_text("int keep_fn(void) { return 1; }\n", encoding="utf-8")
        skip_file.write_text("int skip_fn(void) { return 2; }\n", encoding="utf-8")

        entries = [
            {
                "directory": str(tmpdir),
                "file": str(keep_file),
                "arguments": ["clang", "-c", str(keep_file)],
            },
            {
                "directory": str(tmpdir),
                "file": str(skip_file),
                "arguments": ["clang", "-c", str(skip_file)],
            },
        ]
        compdb.write_text(json.dumps(entries), encoding="utf-8")

        result = run_analyzer(
            [
                f"--compile-commands={compdb}",
                f"--exclude-dir={skip_dir.parent},{tmpdir / 'does-not-exist'}",
            ]
        )
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ --exclude-dir run failed (code {result.returncode})")
            print(output)
            print()
            return False

        if "Excluded 1 input file(s) via --exclude-dir filters" not in output:
            print("  ❌ missing exclude-dir status message")
            print(output)
            print()
            return False
        if "Function: keep_fn" not in output:
            print("  ❌ expected kept function is missing")
            print(output)
            print()
            return False
        if "Function: skip_fn" in output:
            print("  ❌ excluded function is still present in output")
            print(output)
            print()
            return False

    print("  ✅ --exclude-dir input filtering OK\n")
    return True


def check_multi_tu_folder_analysis() -> bool:
    """
    Regression: compile_commands-driven auto-discovery must handle a folder
    that contains multiple translation units and aggregate them in one run.
    """
    print("=== Testing multi-TU folder analysis ===")
    fixture_dir = RUN_CONFIG.test_dir / "test-multi-tu"
    entry_file = fixture_dir / "entry.c"
    worker_file = fixture_dir / "worker.c"

    if not entry_file.exists() or not worker_file.exists():
        print("  ❌ missing multi-TU fixture files")
        print(f"     expected: {entry_file} and {worker_file}")
        print()
        return False

    with tempfile.TemporaryDirectory(prefix="ct_multi_tu_folder_") as tmp:
        tmpdir = Path(tmp)
        compdb = tmpdir / "compile_commands.json"
        entries = [
            {
                "directory": str(fixture_dir.resolve()),
                "file": str(entry_file.resolve()),
                "arguments": ["clang", "-c", str(entry_file.resolve())],
            },
            {
                "directory": str(fixture_dir.resolve()),
                "file": str(worker_file.resolve()),
                "arguments": ["clang", "-c", str(worker_file.resolve())],
            },
        ]
        compdb.write_text(json.dumps(entries), encoding="utf-8")

        result = run_analyzer(
            [f"--compile-commands={compdb}", "--format=json", "--warnings-only"]
        )
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ multi-TU folder run failed (code {result.returncode})")
            print(output)
            print()
            return False

        if "No explicit input files provided: using 2 supported file(s) from compile_commands.json" not in output:
            print("  ❌ missing auto-discovery status for multi-TU folder run")
            print(output)
            print()
            return False

        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError as exc:
            print(f"  ❌ invalid JSON output for multi-TU folder run: {exc}")
            print(result.stdout or "")
            print()
            return False

        expected_inputs = sorted([str(entry_file.resolve()), str(worker_file.resolve())])
        inputs = payload.get("meta", {}).get("inputFiles", [])
        if inputs != expected_inputs:
            print("  ❌ multi-TU inputFiles mismatch")
            print(f"     expected: {expected_inputs}")
            print(f"     got:      {inputs}")
            print()
            return False

        names = {f.get("name", "") for f in payload.get("functions", [])}
        missing_names = sorted([name for name in ["mtu_entry", "mtu_worker"] if name not in names])
        if missing_names:
            print("  ❌ missing multi-TU functions in aggregated output")
            print(f"     missing: {missing_names}")
            print(result.stdout or "")
            print()
            return False

    print("  ✅ multi-TU folder analysis OK\n")
    return True


def check_diagnostic_rule_coverage_regression() -> bool:
    """
    Ensure representative rules are still emitted after analyzer refactors.
    """
    print("=== Testing diagnostic rule coverage regression ===")
    ok = True

    cases = [
        (
            "StackBufferOverflow",
            ["test/bound-storage/bound-storage.c", "--format=json"],
            {"StackBufferOverflow"},
        ),
        (
            "VLAUsage",
            ["test/vla/vla-unknown-stack.c", "--format=json"],
            {"VLAUsage"},
        ),
        (
            "AllocaTooLarge",
            ["test/alloca/oversized-constant.c", "--format=json"],
            {"AllocaTooLarge"},
        ),
        (
            "SizeMinusOneWrite",
            ["test/size-arg/strncpy-size-minus-1.c", "--format=json"],
            {"SizeMinusOneWrite"},
        ),
        (
            "MultipleStoresToStackBuffer",
            ["test/multiple-storage/same-storage.c", "--format=json"],
            {"MultipleStoresToStackBuffer"},
        ),
        (
            "DuplicateIfCondition",
            ["test/diagnostics/duplicate-else-if-basic.c", "--format=json"],
            {"DuplicateIfCondition"},
        ),
        (
            "UninitializedLocalRead",
            ["test/uninitialized-variable/uninitialized-local-basic.c", "--format=json"],
            {"UninitializedLocalRead"},
        ),
        (
            "InvalidBaseReconstruction",
            ["test/offset_of-container_of/container_of_wrong_member_offset_error.c", "--format=json"],
            {"InvalidBaseReconstruction"},
        ),
        (
            "StackPointerEscape",
            ["test/escape-stack/return-buf.c", "--format=json"],
            {"StackPointerEscape"},
        ),
        (
            "ConstParameterNotModified",
            ["test/pointer_reference-const_correctness/readonly-pointer.c", "--format=json"],
            {"ConstParameterNotModified.Pointer", "ConstParameterNotModified.PointerConstOnly"},
        ),
        (
            "ResourceLifetime.MissingRelease",
            [
                "test/resource-lifetime/malloc-missing-release.c",
                "--format=json",
                "--resource-model=models/resource-lifetime/generic.txt",
            ],
            {"ResourceLifetime.MissingRelease"},
        ),
    ]

    for label, args, expected in cases:
        result = run_analyzer(args)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0:
            print(f"  ❌ {label} run failed (code {result.returncode})")
            print(output)
            ok = False
            continue

        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError as exc:
            print(f"  ❌ {label} invalid JSON output: {exc}")
            print(result.stdout or "")
            ok = False
            continue

        diagnostics = payload.get("diagnostics", [])
        rule_ids = {diag.get("ruleId", "") for diag in diagnostics}
        if not any(rule in rule_ids for rule in expected):
            print(f"  ❌ {label} missing expected rule")
            print(f"     expected one of: {sorted(expected)}")
            print(f"     got: {sorted(rule_ids)}")
            ok = False
            continue

        has_loc = False
        for diag in diagnostics:
            location = diag.get("location", {})
            if int(location.get("startLine", 0) or 0) > 0:
                has_loc = True
                break
        if not has_loc:
            print(f"  ❌ {label} has no diagnostic with source location")
            print(result.stdout or "")
            ok = False
            continue

        print(f"  ✅ {label} rule coverage OK")

    print()
    return ok


def check_analyzer_module_unit_tests() -> bool:
    """
    Run fine-grained C++ unit tests for analyzer modules.
    """
    print("=== Testing analyzer module unit tests ===")
    unit_test_bin = RUN_CONFIG.analyzer.parent / "stack_usage_analyzer_unit_tests"
    if not unit_test_bin.exists():
        print("  [info] unit test binary not found, skipping")
        print(f"     expected: {unit_test_bin}")
        print("     enable with: cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON")
        print("     then build:   cmake --build build --target stack_usage_analyzer_unit_tests")
        print()
        return True

    repo_root = Path(__file__).resolve().parent
    result = subprocess.run(
        [str(unit_test_bin), str(repo_root.resolve())], capture_output=True, text=True
    )
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode != 0:
        print(f"  ❌ analyzer module unit tests failed (code {result.returncode})")
        print(output)
        print()
        return False

    print("  ✅ analyzer module unit tests OK")
    if output.strip():
        print(output.rstrip())
    print()
    return True


def check_file(c_path: Path):
    """
    Check that, for this file, all expectations are present in the analyzer output.
    """
    report_lines = [f"=== Testing {c_path} ==="]
    (
        expectations,
        negative_expectations,
        stack_limit,
        resource_model,
        escape_model,
        buffer_model,
        strict_diag_count,
    ) = extract_expectations(c_path)
    strict_enabled = (
        strict_diag_count if strict_diag_count is not None else _default_strict_diagnostic_count(c_path)
    )
    if not expectations and not negative_expectations and not strict_enabled:
        report_lines.append("  (no expectations found, skipping)")
        return True, 0, 0, "\n".join(report_lines) + "\n\n"

    def evaluate_pass(pass_name: str, analyzer_output: str):
        pass_lines = [f"  [pass: {pass_name}]"]
        norm_output = normalize(analyzer_output)
        output_index = _build_output_diagnostic_index_by_location(analyzer_output)

        pass_ok = True
        pass_total = len(expectations) + len(negative_expectations)
        pass_passed = 0

        for idx, exp in enumerate(expectations, start=1):
            norm_exp = normalize(exp)
            matched = norm_exp in norm_output
            if not matched:
                # Optimization: only normalize the body once, then vary the
                # location prefix.  Avoids ~184 full normalize() calls per
                # non-matching expectation.
                exp_lines = exp.splitlines()
                loc_match = _RE_LOCATION.match(exp_lines[0]) if exp_lines else None
                if loc_match:
                    norm_body = normalize("\n".join(exp_lines[1:])) if len(exp_lines) > 1 else ""
                    base_line = int(loc_match.group(1))
                    base_col = int(loc_match.group(2))
                    for line_delta in range(-18, 19):
                        for col_delta in (-2, -1, 0, 1, 2):
                            if line_delta == 0 and col_delta == 0:
                                continue
                            cl = base_line + line_delta
                            cc = base_col + col_delta
                            if cl <= 0 or cc < 0:
                                continue
                            alt_loc = f"at line {cl}, column {cc}"
                            candidate = f"{alt_loc}\n{norm_body}" if norm_body else alt_loc
                            if candidate in norm_output:
                                matched = True
                                break
                        if matched:
                            break
            if not matched and _expectation_matches_by_location_and_headlines(exp, output_index):
                matched = True

            if matched:
                pass_lines.append(f"  ✅ ({pass_name}) expectation #{idx} FOUND")
                pass_passed += 1
            else:
                pass_lines.append(f"  ❌ ({pass_name}) expectation #{idx} MISSING")
                pass_lines.append("----- Expected block -----")
                pass_lines.append(exp)
                pass_lines.append("----- Analyzer output (normalized) -----")
                pass_lines.append(f"<{norm_output}>")
                pass_lines.append("---------------------------")
                pass_ok = False

        for idx, neg in enumerate(negative_expectations, start=1):
            norm_neg = normalize(neg)
            if norm_neg and norm_neg not in norm_output:
                pass_lines.append(
                    f"  ✅ ({pass_name}) negative expectation #{idx} NOT FOUND (as expected)"
                )
                pass_passed += 1
            else:
                pass_lines.append(f"  ❌ ({pass_name}) negative expectation #{idx} FOUND (unexpected)")
                pass_lines.append("----- Forbidden text -----")
                pass_lines.append(neg)
                pass_lines.append("----- Analyzer output (normalized) -----")
                pass_lines.append(f"<{norm_output}>")
                pass_lines.append("---------------------------")
                pass_ok = False

        if strict_enabled:
            pass_total += 1
            expected_warning_error = sum(
                1 for exp in expectations if _expectation_is_warning_or_error(exp)
            )
            actual_warning_error = _parse_total_warning_error_count(analyzer_output)
            if actual_warning_error is None:
                pass_lines.append(
                    f"  ❌ ({pass_name}) strict diagnostic count check: summary line missing"
                )
                pass_lines.append("----- Analyzer output -----")
                pass_lines.append(analyzer_output.strip())
                pass_lines.append("---------------------------")
                pass_ok = False
            elif actual_warning_error == expected_warning_error:
                pass_lines.append(
                    f"  ✅ ({pass_name}) strict diagnostic count match ({actual_warning_error} warning/error)"
                )
                pass_passed += 1
            else:
                pass_lines.append(f"  ❌ ({pass_name}) strict diagnostic count mismatch")
                pass_lines.append(
                    f"     expected warning/error from comments: {expected_warning_error}"
                )
                pass_lines.append(
                    f"     actual warning/error in analyzer output: {actual_warning_error}"
                )
                pass_lines.append("     hint: add missing // at line ... expectation blocks")
                pass_ok = False

        return pass_ok, pass_total, pass_passed, pass_lines

    all_ok = True
    total = 0
    passed = 0

    baseline_output = run_analyzer_on_file(
        c_path,
        stack_limit=stack_limit,
        resource_model=resource_model,
        escape_model=escape_model,
        buffer_model=buffer_model,
    )
    base_ok, base_total, base_passed, base_lines = evaluate_pass("default", baseline_output)
    report_lines.extend(base_lines)
    all_ok = all_ok and base_ok
    total += base_total
    passed += base_passed

    if _runner_has_explicit_smt_args():
        report_lines.append("  [info] dedicated smt-z3 pass skipped (runner already has --smt args)")
    else:
        smt_rules_csv = ",".join(_all_smt_rules())
        smt_output = run_analyzer_on_file(
            c_path,
            stack_limit=stack_limit,
            resource_model=resource_model,
            escape_model=escape_model,
            buffer_model=buffer_model,
            extra_args=(
                "--smt=on",
                "--smt-backend=z3",
                "--smt-mode=single",
                f"--smt-rules={smt_rules_csv}",
            ),
        )
        smt_ok, smt_total, smt_passed, smt_lines = evaluate_pass("smt-z3", smt_output)
        report_lines.extend(smt_lines)
        all_ok = all_ok and smt_ok
        total += smt_total
        passed += smt_passed

    return all_ok, total, passed, "\n".join(report_lines) + "\n\n"


def _run_check_parallel(dispatch, fn):
    """Run a check function in a worker thread with output capture."""
    dispatch.register_thread()
    try:
        ok = fn()
    finally:
        output = dispatch.unregister_thread()
    return ok, output


def main() -> int:
    cli = parse_args()
    RUN_CONFIG.jobs = max(1, cli.jobs)
    RUN_CONFIG.cache_enabled = not cli.no_cache
    RUN_CONFIG.cache_dir = Path(cli.cache_dir)
    env_extra_args = shlex.split(os.environ.get("CORETRACE_RUN_TEST_EXTRA_ANALYZER_ARGS", ""))
    RUN_CONFIG.extra_analyzer_args = tuple([*cli.analyzer_arg, *env_extra_args])

    if cli.clear_cache and RUN_CONFIG.cache_dir.exists():
        shutil.rmtree(RUN_CONFIG.cache_dir, ignore_errors=True)

    total_tests = 0
    passed_tests = 0

    def record_ok(ok: bool):
        nonlocal total_tests, passed_tests
        total_tests += 1
        if ok:
            passed_tests += 1
        return ok

    # Thread-safe check functions — order is preserved for output.
    # check_docker_entrypoint_guardrails mutates os.environ and is
    # therefore excluded from the parallel batch and run sequentially
    # after the pool completes.
    parallel_checks = [
        check_help_flags,
        check_analyzer_module_unit_tests,
        check_multi_file_json,
        check_multi_file_total_summary,
        check_multi_file_failure,
        check_cli_parsing_and_filters,
        check_only_func_uninitialized,
        check_warnings_only_filters_function_listing,
        check_uninitialized_verbose_ctor_trace,
        check_uninitialized_unsummarized_defined_bool_out_param,
        check_uninitialized_optional_receiver_index_repro,
        check_unknown_alloca_virtual_callback_escape,
        check_compdb_as_default_input_source,
        check_exclude_dir_filter,
        check_multi_tu_folder_analysis,
        check_resource_lifetime_cross_tu,
        check_uninitialized_cross_tu,
        check_null_deref_nested_inter_tu,
        check_integer_overflow_advanced_inter_tu,
        check_use_after_free_advanced_inter_tu,
        check_escape_model_rejects_unsupported_brackets,
        check_human_vs_json_parity,
        check_diagnostic_rule_coverage_regression,
    ]
    # Env-mutating check — must run outside the parallel pool.
    sequential_checks = [
        check_docker_entrypoint_guardrails,
    ]

    global_ok = True

    if RUN_CONFIG.jobs > 1:
        global _PARALLEL_PHASE
        _PARALLEL_PHASE = True
        # Parallel execution: capture each function's stdout via
        # _ThreadDispatchStdout so output is printed in deterministic order.
        dispatch = _ThreadDispatchStdout(sys.stdout)
        original_stdout = sys.stdout
        sys.stdout = dispatch
        try:
            with ThreadPoolExecutor(max_workers=RUN_CONFIG.jobs) as executor:
                futures = [
                    executor.submit(_run_check_parallel, dispatch, fn)
                    for fn in parallel_checks
                ]
                results = [f.result() for f in futures]
        finally:
            sys.stdout = original_stdout
            _PARALLEL_PHASE = False

        for ok, output in results:
            sys.stdout.write(output)
            if not record_ok(ok):
                global_ok = False
    else:
        for fn in parallel_checks:
            if not record_ok(fn()):
                global_ok = False

    # Sequential-only checks (env mutation, filesystem side effects, etc.).
    for fn in sequential_checks:
        if not record_ok(fn()):
            global_ok = False

    # Per-fixture file checks (already supported parallelism via --jobs).
    c_files = collect_fixture_sources()
    if not c_files:
        print(f"No .c/.cpp files found under {RUN_CONFIG.test_dir}")
        return 0 if global_ok else 1

    if RUN_CONFIG.jobs <= 1:
        for f in c_files:
            ok, total, passed, report = check_file(f)
            print(report, end="")
            passed_tests += passed
            total_tests += total
            if not ok:
                global_ok = False
    else:
        with ThreadPoolExecutor(max_workers=RUN_CONFIG.jobs) as executor:
            results = list(executor.map(check_file, c_files))
        for ok, total, passed, report in results:
            print(report, end="")
            passed_tests += passed
            total_tests += total
            if not ok:
                global_ok = False

    if global_ok:
        print("✅ All tests passed.")
        print(f"✅ Passed {passed_tests}/{total_tests} tests.")
        return 0
    else:
        print("❌ Some tests failed.")
        print(f"❌ Passed {passed_tests}/{total_tests} tests.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
