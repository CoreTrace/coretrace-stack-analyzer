#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
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
import textwrap
import uuid
from dataclasses import dataclass
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Optional

DEFAULT_ANALYZER = Path("./build/stack_usage_analyzer")
DEFAULT_TEST_DIR = Path("test")
DEFAULT_CACHE_DIR = Path(".cache/run_test")


# A single analyzer invocation that outlives this budget is treated as hung.
# Without it one stuck invocation blocks its worker thread forever, and the whole
# suite stalls until the CI step timeout kills it with no indication of which
# fixture was responsible.
DEFAULT_ANALYZER_TIMEOUT = 300.0

# Distinct from any exit code the analyzer itself produces, so a timeout is never
# mistaken for an ordinary failure.
ANALYZER_TIMEOUT_RETURNCODE = -9001

# The smt-z3 fixture pass checks what the solver can prove, not how fast it proves it. At the
# analyzer's default budget of 50 ms per query, a proof that takes 7 ms on a laptop ran out of
# time on a loaded CI runner (#144), and the fixture kept a report the solver removes.
SMT_FIXTURE_TIMEOUT_MS = 1000


@dataclass
class TestRunConfig:
    analyzer: Path = DEFAULT_ANALYZER
    test_dir: Path = DEFAULT_TEST_DIR
    cache_dir: Path = DEFAULT_CACHE_DIR
    jobs: int = 1
    cache_enabled: bool = True
    extra_analyzer_args: tuple[str, ...] = ()
    analyzer_timeout: Optional[float] = DEFAULT_ANALYZER_TIMEOUT


RUN_CONFIG = TestRunConfig()
_CACHE_LOCK = threading.Lock()
_MEM_CACHE = {}
_FILE_HASH_CACHE = {}

def _get_file_hash(p: Path) -> str:
    path_str = str(p)
    try:
        st = p.stat()
    except OSError:
        return ""
    
    # Use st_mtime_ns as a cache key for the hash
    cache_key = (path_str, st.st_mtime_ns, st.st_size)
    with _CACHE_LOCK:
        if cache_key in _FILE_HASH_CACHE:
            return _FILE_HASH_CACHE[cache_key]
        
    try:
        h = hashlib.sha256(p.read_bytes()).hexdigest()
    except OSError:
        h = ""
        
    with _CACHE_LOCK:
        _FILE_HASH_CACHE[cache_key] = h
    return h
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
_RE_STRICT_DETAILS = re.compile(r"//\s*strict-expectation-details\s*[:=]\s*(\S+)", re.IGNORECASE)
# `// [smt-z3] not contains: ...` or `// [default] at line ...`: an expectation for one pass.
_RE_PASS_SCOPE = re.compile(r"//\s*\[([A-Za-z0-9_-]+)\]\s*(?=at line|not contains:)")
_EXPECTATION_PASSES = ("default", "smt-z3")


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
        "--analyzer-timeout",
        type=float,
        default=DEFAULT_ANALYZER_TIMEOUT,
        help=(
            "Seconds a single analyzer invocation may run before it is killed and "
            f"reported as hung (default: {DEFAULT_ANALYZER_TIMEOUT:g}). 0 disables the timeout."
        ),
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
        h = _get_file_hash(p)
        if h:
            deps.append([str(p), h])
    return deps


def _cache_key_for_args(args, env_overrides: Optional[dict[str, str]] = None):
    payload = {
        "analyzer": str(RUN_CONFIG.analyzer.resolve()),
        "args": list(args),
        "cwd": str(Path.cwd()),
        "deps": _collect_cache_dependencies(args),
        "env": dict(sorted((env_overrides or {}).items())),
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
    Both "// at line" and "// not contains:" accept a pass prefix, "// [default] ..." or
    "// [smt-z3] ...", restricting the expectation to that pass. Expectations are returned as
    (scope, text) pairs, scope None meaning every pass.
    """
    expectations = []
    negative_expectations = []
    unknown_scopes = []
    stack_limit = None
    resource_model = None
    escape_model = None
    buffer_model = None
    strict_diag_count = None
    strict_details = None
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
        details_match = _RE_STRICT_DETAILS.match(stripped)
        if details_match:
            parsed = parse_bool_directive(details_match.group(1))
            if parsed is not None:
                strict_details = parsed
            i += 1
            continue

        scope = None
        scope_match = _RE_PASS_SCOPE.match(stripped)
        if scope_match:
            scope = scope_match.group(1)
            if scope not in _EXPECTATION_PASSES:
                unknown_scopes.append(scope)
            stripped = "// " + stripped[scope_match.end():]

        stripped_line = stripped
        if stripped_line.startswith("// not contains:"):
            negative = stripped_line[len("// not contains:"):].strip()
            if negative:
                negative_expectations.append((scope, negative))
            i += 1
            continue

        # Start of an expectation block
        if stripped.startswith("// at line"):
            comment_block = [stripped]
            i += 1
            # Collect all following "// ..." lines
            # A scoped line starts its own expectation, even without a blank line before it.
            while (
                i < n
                and lines[i].lstrip().startswith("//")
                and not _RE_PASS_SCOPE.match(lines[i].lstrip())
            ):
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
            expectations.append((scope, expectation_text))
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
        strict_details,
        unknown_scopes,
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


def _spawn_analyzer(cmd, env) -> subprocess.CompletedProcess:
    """
    Run the analyzer under the configured timeout.

    A hung invocation is reported as a failed run naming the command, so the
    suite fails fast on the responsible fixture instead of stalling.
    """
    try:
        return subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            env=env,
            timeout=RUN_CONFIG.analyzer_timeout,
        )
    except subprocess.TimeoutExpired as expired:
        def _decode(stream) -> str:
            if not stream:
                return ""
            return stream if isinstance(stream, str) else stream.decode("utf-8", "replace")

        return subprocess.CompletedProcess(
            args=cmd,
            returncode=ANALYZER_TIMEOUT_RETURNCODE,
            stdout=_decode(expired.stdout),
            stderr=_decode(expired.stderr)
            + f"\n[run_test] analyzer timed out after {expired.timeout:g}s: {shlex.join(cmd)}\n",
        )


def run_analyzer(args, env_overrides: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess:
    """
    Run analyzer with custom args and return the CompletedProcess.
    """
    effective_args = _effective_analyzer_args(args)
    cmd = [str(RUN_CONFIG.analyzer)] + effective_args
    key = _cache_key_for_args(effective_args, env_overrides)

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

    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)
    result = _spawn_analyzer(cmd, env)
    # A timeout says nothing about what the analyzer would have produced, so it
    # must not be memoised as if it were a result.
    if result.returncode == ANALYZER_TIMEOUT_RETURNCODE:
        return result
    with _CACHE_LOCK:
        _MEM_CACHE[key] = {
            "returncode": result.returncode,
            "stdout": result.stdout or "",
            "stderr": result.stderr or "",
        }
    _cache_store(key, result)
    return result


def run_analyzer_uncached(args, env_overrides: Optional[dict[str, str]] = None) -> subprocess.CompletedProcess:
    """
    Run analyzer with custom args and bypass run_test.py cache layer.
    Useful for checks that assert filesystem side effects.
    """
    cmd = [str(RUN_CONFIG.analyzer)] + _effective_analyzer_args(args)
    env = os.environ.copy()
    if env_overrides:
        env.update(env_overrides)
    return _spawn_analyzer(cmd, env)


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


_SMT_BACKEND_UNAVAILABLE = "is not available in this build"


def check_smt_unavailable_backend_warning() -> bool:
    """
    `--smt=on` must say on stderr when a backend it uses is not compiled in. This run also
    needs Z3: without it the dedicated smt-z3 fixture pass silently checks nothing.
    """
    print("=== Testing SMT backend availability ===")
    if _runner_has_explicit_smt_args():
        print("  [info] skipped (runner already has --smt args)")
        print()
        return True

    fixture = str(fixture_path_with_fallback("integer-overflow/nsw-flag-must-not-discharge-itself.c"))
    # (analyzer arguments, backend that must be reported once, or None for no warning)
    cases = [
        (["--smt=on", "--smt-backend=z3"], None),
        (["--smt=on", "--smt-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-backend=Interval"], None),
        (["--smt=on", "--smt-mode=portfolio", "--smt-secondary-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-mode=cross-check", "--smt-secondary-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-mode=dual-consensus", "--smt-secondary-backend=cvc5"], "cvc5"),
        (["--smt=on", "--smt-mode=single", "--smt-secondary-backend=cvc5"], None),
        # One backend named twice, in two casings, is one backend.
        (
            ["--smt=on", "--smt-mode=portfolio", "--smt-backend=CVC5", "--smt-secondary-backend=cvc5"],
            "CVC5",
        ),
        (["--smt-backend=cvc5", "--smt=off"], None),
    ]
    ok = True
    for args, backend in cases:
        result = run_analyzer([*args, fixture])
        stderr = result.stderr or ""
        warnings = stderr.count(_SMT_BACKEND_UNAVAILABLE)
        label = " ".join(args)
        if result.returncode != 0:
            print(f"  ❌ {label}: exited {result.returncode}; the warning must not change it")
            ok = False
        elif _SMT_BACKEND_UNAVAILABLE in (result.stdout or ""):
            print(f"  ❌ {label}: the warning must go to stderr, not stdout")
            ok = False
        elif backend is None and warnings:
            print(f"  ❌ {label}: unexpected warning")
            if "SMT backend 'z3'" in stderr:
                print(
                    "     this analyzer has no Z3: install libz3-dev and pkg-config (Debian/Ubuntu)"
                    " or `brew install z3`, then re-run cmake and rebuild"
                )
            ok = False
        elif backend is not None and (warnings != 1 or f"SMT backend '{backend}'" not in stderr):
            print(f"  ❌ {label}: expected one warning for '{backend}', got {warnings}")
            ok = False
    if ok:
        print("  ✅ SMT backend availability OK")
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
        ("--compile-ir-format", "Missing argument for --compile-ir-format"),
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
        (["--compile-ir-format=foo", str(sample)], "Invalid --compile-ir-format value:"),
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
            ("--compile-ir-format=bc", [str(sample_c), "--compile-ir-format=bc"], ["Function:"], "text"),
            ("--compile-ir-format=ll", [str(sample_c), "--compile-ir-format=ll"], ["Function:"], "text"),
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


def check_compile_ir_format_switch() -> bool:
    """
    Validate that --compile-ir-format selects the expected source compile IR path
    and that cache keys invalidate correctly when switching format.
    """
    print("=== Testing compile IR format switch ===")
    sample_c = RUN_CONFIG.test_dir / "alloca/oversized-constant.c"
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_compile_ir_format_") as tmp:
        cache_dir = Path(tmp) / "compile-ir-cache"
        cache_arg = f"--compile-ir-cache-dir={cache_dir}"

        def run_and_capture(fmt: str) -> tuple[bool, str]:
            result = run_analyzer_uncached(
                [str(sample_c), "--timing", cache_arg, f"--compile-ir-format={fmt}"]
            )
            output = (result.stdout or "") + (result.stderr or "")
            if result.returncode != 0:
                print(f"  ❌ --compile-ir-format={fmt} failed (code {result.returncode})")
                print(output)
                return False, output
            return True, output

        bc_ok, bc_output = run_and_capture("bc")
        ok = ok and bc_ok
        if bc_ok:
            if "Bitcode parse done in" not in bc_output:
                print("  ❌ --compile-ir-format=bc did not use bitcode parse path")
                print(bc_output)
                ok = False
            else:
                print("  ✅ --compile-ir-format=bc uses bitcode parse path")

        ll_ok, ll_output = run_and_capture("ll")
        ok = ok and ll_ok
        if ll_ok:
            if "IR parse done in" not in ll_output:
                print("  ❌ --compile-ir-format=ll did not use textual IR parse path")
                print(ll_output)
                ok = False
            elif "Bitcode parse done in" in ll_output:
                print("  ❌ --compile-ir-format=ll unexpectedly used bitcode parse path")
                print(ll_output)
                ok = False
            else:
                print("  ✅ --compile-ir-format=ll uses textual IR parse path")

        # Re-run BC with the same cache directory to ensure format switch is
        # versioned in cache identity and does not pin the LL parse path.
        bc2_ok, bc2_output = run_and_capture("bc")
        ok = ok and bc2_ok
        if bc2_ok:
            if "Bitcode parse done in" not in bc2_output:
                print("  ❌ cache invalidation failed when switching back to bc")
                print(bc2_output)
                ok = False
            elif "IR parse done in" in bc2_output:
                print("  ❌ bc run unexpectedly reused textual IR parse path")
                print(bc2_output)
                ok = False
            else:
                print("  ✅ cache invalidation across bc/ll switch OK")

    print()
    return ok


def check_pipeline_subscriber_rollout_parity() -> bool:
    """
    Integration check: diagnostics must remain stable when toggling the
    subscriber rollout flag.
    """
    print("=== Testing pipeline subscriber rollout parity ===")
    fixtures = [
        RUN_CONFIG.test_dir / "alloca/oversized-constant.c",
        RUN_CONFIG.test_dir / "resource-lifetime/local-double-release.c",
        RUN_CONFIG.test_dir / "integer-overflow/cross-tu-tricky-use.c",
        RUN_CONFIG.test_dir / "uninitialized-variable/uninitialized-local-unused.c",
        RUN_CONFIG.test_dir / "diagnostics/duplicate-else-if-basic.c",
    ]

    ok = True
    for fixture in fixtures:
        args = [str(fixture), "--warnings-only", "--format=json"]
        baseline = run_analyzer(args)
        baseline_output = (baseline.stdout or "") + (baseline.stderr or "")
        if baseline.returncode != 0:
            print(f"  ❌ baseline run failed for {fixture} (code {baseline.returncode})")
            print(baseline_output)
            ok = False
            continue
        try:
            baseline_payload = json.loads(baseline.stdout or "")
        except json.JSONDecodeError as exc:
            print(f"  ❌ baseline JSON parse failed for {fixture}: {exc}")
            print(baseline.stdout or "")
            ok = False
            continue

        subscriber = run_analyzer(args, env_overrides={"CTRACE_PIPELINE_SUBSCRIBERS": "1"})
        subscriber_output = (subscriber.stdout or "") + (subscriber.stderr or "")
        if subscriber.returncode != 0:
            print(f"  ❌ subscriber run failed for {fixture} (code {subscriber.returncode})")
            print(subscriber_output)
            ok = False
            continue
        try:
            subscriber_payload = json.loads(subscriber.stdout or "")
        except json.JSONDecodeError as exc:
            print(f"  ❌ subscriber JSON parse failed for {fixture}: {exc}")
            print(subscriber.stdout or "")
            ok = False
            continue

        baseline_norm = json.dumps(baseline_payload, sort_keys=True, separators=(",", ":"))
        subscriber_norm = json.dumps(subscriber_payload, sort_keys=True, separators=(",", ":"))
        if baseline_norm != subscriber_norm:
            print(f"  ❌ parity mismatch with subscriber rollout for {fixture}")
            print("  --- baseline ---")
            print(baseline.stdout or "")
            print("  --- subscriber ---")
            print(subscriber.stdout or "")
            ok = False
            continue

        print(f"  ✅ parity OK with subscriber rollout for {fixture}")

    print()
    return ok


def check_pipeline_timing_traversal_instrumentation() -> bool:
    """
    Integration check: timing output must include traversal instrumentation
    and derived-artifact metadata.
    """
    print("=== Testing pipeline traversal instrumentation ===")
    sample = RUN_CONFIG.test_dir / "alloca/oversized-constant.c"
    result = run_analyzer_uncached(
        [str(sample), "--timing", "--quiet"],
        env_overrides={"CTRACE_PIPELINE_SUBSCRIBERS": "1"},
    )
    output = (result.stdout or "") + (result.stderr or "")

    if result.returncode != 0:
        print(f"  ❌ analyzer failed (code {result.returncode})")
        print(output)
        print()
        return False

    required = [
        "IR facts mode: subscriber",
        "Derived artifacts schema: derived-module-artifacts-v1",
        "Traversal estimate detail: step='Stack buffer overflows'",
        "Traversal estimate by model:",
    ]

    for needle in required:
        if needle not in output:
            print(f"  ❌ missing traversal instrumentation token: {needle}")
            print(output)
            print()
            return False

    print("  ✅ traversal instrumentation output OK\n")
    return True


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


def check_resource_model_across_compile_directories() -> bool:
    """
    A resource model given by a relative path is loaded, and used, whatever the job count, while
    the files compile from compile-command directories of their own. Each directory holds the one
    header its file includes through a relative -I, so a file compiles only from its own
    directory; the analyzer must come back to the directory it started from, where the model and
    the relative cache directories are. Every run starts with empty caches, and a leak that only
    the model defines must be reported in every file. The relative paths must be anchored to the
    start directory, and parallel jobs must not change the working directory for files whose
    compile commands need no directory of their own.
    """
    print("=== Testing the resource model across compile-command directories ===")
    unit_count = 8
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_resource_model_cwd_") as tmp:
        root = Path(tmp)
        project = root / "project"
        entries = []
        for i in range(unit_count):
            unit_dir = project / f"unit{i}"
            (unit_dir / "include").mkdir(parents=True)
            (unit_dir / "include" / f"unit{i}.h").write_text(
                "void* ct_open_widget(void);\nvoid ct_close_widget(void* w);\n"
            )
            source = unit_dir / f"unit{i}.c"
            source.write_text(
                f'#include "unit{i}.h"\n\n'
                f"int leak_widget_{i}(void)\n{{\n    void* w = ct_open_widget();\n"
                "    return w != 0;\n}\n\n"
                f"int keep_widget_{i}(void)\n{{\n    void* w = ct_open_widget();\n"
                "    ct_close_widget(w);\n    return 0;\n}\n"
            )
            entries.append(
                {
                    "directory": str(unit_dir),
                    "file": str(source),
                    "arguments": ["cc", "-Iinclude", "-c", str(source), "-o", f"unit{i}.o"],
                }
            )
        compdb = project / "compile_commands.json"
        compdb.write_text(json.dumps(entries, indent=2))
        run_dir = root / "run"
        run_dir.mkdir()
        (run_dir / "widget-model.txt").write_text(
            "acquire_ret ct_open_widget Widget\nrelease_arg ct_close_widget 0 Widget\n"
        )
        sources = [entry["file"] for entry in entries]
        expected = {f"leak_widget_{i}" for i in range(unit_count)}

        analyzer = str(Path(RUN_CONFIG.analyzer).resolve())

        def run_from_start_dir(args):
            try:
                return subprocess.run(
                    [analyzer, *args],
                    cwd=run_dir,
                    capture_output=True,
                    text=True,
                    timeout=RUN_CONFIG.analyzer_timeout,
                )
            except subprocess.TimeoutExpired:
                return None

        start_dir = run_dir.resolve()
        result = run_from_start_dir(
            [
                "--print-effective-config",
                f"--compile-commands={compdb}",
                "--resource-model=widget-model.txt",
                "--compile-ir-cache-dir=caches-config/compile-ir",
                sources[0],
            ]
        )
        printed = (result.stdout + result.stderr) if result else ""
        for line in (
            f"resource-model: {start_dir / 'widget-model.txt'}",
            f"compile-ir-cache-dir: {start_dir / 'caches-config' / 'compile-ir'}",
        ):
            if line in printed:
                print(f"  ✅ effective configuration: {line.split(':')[0]} anchored to start")
            else:
                print(f"  ❌ effective configuration lacks '{line}'")
                ok = False

        absolute_compdb = project / "compile_commands_absolute.json"
        absolute_compdb.write_text(
            json.dumps(
                [
                    {**entry, "arguments": ["cc", f"-I{Path(entry['directory']) / 'include'}",
                                            *entry["arguments"][2:]]}
                    for entry in entries
                ],
                indent=2,
            )
        )
        # Automatic jobs, the default, run in parallel too.
        result = run_from_start_dir(
            ["--timing", f"--compile-commands={absolute_compdb}",
             "--compile-ir-cache-dir=caches-timing/compile-ir", *sources]
        )
        timing = (result.stdout + result.stderr) if result else ""
        if result is None or result.returncode != 0:
            print("  ❌ automatic jobs with absolute include paths: run failed")
            ok = False
        elif "input.compiler.invoke.cwd" in timing:
            print("  ❌ automatic jobs changed the working directory for absolute commands")
            ok = False
        else:
            print("  ✅ automatic jobs kept the working directory for absolute commands")

        for mode in ([], ["--jobs=1"], ["--jobs=4"]):
            label = mode[0] if mode else "default jobs"
            for attempt in range(1, 4):
                caches = f"caches-{label.replace('=', '').replace(' ', '-')}-{attempt}"
                cmd = [
                    analyzer,
                    "--format=json",
                    f"--compile-commands={compdb}",
                    "--resource-model=widget-model.txt",
                    f"--compile-ir-cache-dir={caches}/compile-ir",
                    f"--resource-summary-cache-dir={caches}/resource",
                    *mode,
                    *sources,
                ]
                result = run_from_start_dir(cmd[1:])
                if result is None:
                    print(f"  ❌ {label}, run {attempt}: timed out")
                    ok = False
                    continue
                problems = []
                if "cannot open model file" in result.stderr:
                    problems.append("the model failed to load")
                try:
                    diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
                except json.JSONDecodeError:
                    diagnostics = None
                    problems.append(f"no JSON output (exit {result.returncode})")
                if diagnostics is not None:
                    found = {
                        d.get("location", {}).get("function")
                        for d in diagnostics
                        if d.get("ruleId") == "ResourceLifetime.MissingRelease"
                    }
                    if found != expected:
                        problems.append(f"leaks reported in {sorted(found)}")
                for cache in ("compile-ir", "resource"):
                    if not (run_dir / caches / cache).is_dir():
                        problems.append(f"no {cache} cache under the start directory")
                strays = sorted(str(p.relative_to(root)) for p in project.rglob(caches))
                if strays:
                    problems.append(f"caches under the compile directories: {strays}")
                if problems:
                    print(f"  ❌ {label}, run {attempt}: {'; '.join(problems)}")
                    ok = False
                else:
                    print(f"  ✅ {label}, run {attempt}: model used in {unit_count} files")
    print()
    return ok


def check_resource_lifetime_cross_tu() -> bool:
    """
    Regression: cross-TU resource summaries must propagate acquire/release effects
    across separate translation units.
    """
    print("=== Testing resource lifetime cross-TU summaries ===")
    model = "models/resource-lifetime/generic.txt"
    with tempfile.TemporaryDirectory(prefix="ct_resource_cross_tu_") as tmp:
        tmpdir = Path(tmp)
        compile_cache_dir = tmpdir / "compile-ir-cache"
        resource_cache_dir = tmpdir / "resource-cache"
        compile_cache_arg = f"--compile-ir-cache-dir={compile_cache_dir}"
        default_resource_cache_arg = f"--resource-summary-cache-dir={resource_cache_dir}"

        wrapper_use = RUN_CONFIG.test_dir / "resource-lifetime/cross-tu-wrapper-use.c"
        result = run_analyzer(
            [str(wrapper_use), f"--resource-model={model}", "--warnings-only", compile_cache_arg, default_resource_cache_arg]
        )
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
                compile_cache_arg,
                default_resource_cache_arg,
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
            [
                str(wrapper_def),
                str(wrapper_leak),
                f"--resource-model={model}",
                "--warnings-only",
                compile_cache_arg,
                default_resource_cache_arg,
            ]
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
            [
                str(ret_def),
                str(ret_use),
                f"--resource-model={model}",
                "--warnings-only",
                compile_cache_arg,
                default_resource_cache_arg,
            ]
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
                compile_cache_arg,
                default_resource_cache_arg,
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

        cache_dir = tmpdir / "resource-summary-disk-cache"
        result = run_analyzer_uncached(
            [
                str(ret_def),
                str(ret_use),
                f"--resource-model={model}",
                f"--resource-summary-cache-dir={cache_dir}",
                "--warnings-only",
                compile_cache_arg,
            ]
        )
        output = (result.stdout or "") + (result.stderr or "")
        if not expect_returncode_zero(result, output, "return cross-TU cache run failed"):
            return False
        if not list(cache_dir.glob("*.json")):
            return fail_check("cross-TU cache directory was not populated", output)

        memory_only_cache_dir = tmpdir / "resource-summary-memory-only-cache"
        result = run_analyzer_uncached(
            [
                str(ret_def),
                str(ret_use),
                f"--resource-model={model}",
                "--resource-summary-cache-memory-only",
                f"--resource-summary-cache-dir={memory_only_cache_dir}",
                "--warnings-only",
                compile_cache_arg,
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


def check_ownership_cross_tu() -> bool:
    """
    Cross-TU transformer summaries: a wrapper that always releases clears the
    obligation in its caller; one that releases only sometimes leaves a possible
    leak. The second run must come from the summary cache and print the same.
    """
    print("=== Testing resource ownership cross-TU transformers ===")
    model = "models/resource-lifetime/generic.txt"
    fixtures = RUN_CONFIG.test_dir / "resource-lifetime"
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_ownership_cross_tu_") as tmp:
        tmpdir = Path(tmp)
        common = [
            f"--resource-model={model}",
            "--warnings-only",
            f"--compile-ir-cache-dir={tmpdir / 'compile-ir-cache'}",
            f"--resource-summary-cache-dir={tmpdir / 'resource-cache'}",
        ]
        cases = [
            ("release-always", "use_release_always", False),
            ("release-sometimes", "use_release_sometimes", True),
        ]
        for name, func, expect_leak in cases:
            args = [str(fixtures / f"cross-tu-{name}-def.c"), str(fixtures / f"cross-tu-{name}-use.c")] + common
            outputs = []
            for run in ("first", "cached"):
                result = run_analyzer_uncached(args)
                if result.returncode != 0:
                    print(f"  ❌ {name} ({run}) failed (code {result.returncode})")
                    print((result.stdout or "") + (result.stderr or ""))
                    return False
                # stdout only: stderr carries a per-process log prefix.
                outputs.append(result.stdout or "")
            leak_text = "potential resource leak: 'GenericHandle' acquired in handle 'h'"
            has_leak = leak_text in outputs[0]
            if has_leak != expect_leak:
                print(f"  ❌ {name}: leak {'expected' if expect_leak else 'not expected'} in {func}")
                print(outputs[0])
                ok = False
            elif expect_leak and "may leave the function without being released" not in outputs[0]:
                print(f"  ❌ {name}: the leak must be reported as a possible (partial) leak")
                print(outputs[0])
                ok = False
            if outputs[0] != outputs[1]:
                print(f"  ❌ {name}: cached run differs from the first run")
                ok = False
    if ok:
        print("  ✅ resource ownership cross-TU transformers OK\n")
    return ok


def check_ownership_wrapper_metadata() -> bool:
    """Wrappers preserve uncertainty and conditional acquisitions, including cached summaries."""
    print("=== Testing ownership wrapper metadata ===")
    fixtures = RUN_CONFIG.test_dir / "resource-lifetime"
    definition = fixtures / "wrapper-metadata-def.c"
    caller = fixtures / "wrapper-metadata-use.c"
    model = fixtures / "models" / "wrapper-metadata.txt"
    with tempfile.TemporaryDirectory(prefix="ct_wrapper_metadata_") as tmp:
        tmpdir = Path(tmp)
        local = tmpdir / "local.c"
        local.write_text(definition.read_text() + "\n" + caller.read_text())
        cache = tmpdir / "summaries"
        common = [
            f"--resource-model={model}", "--warnings-only",
            f"--resource-summary-cache-dir={cache}",
            f"--compile-ir-cache-dir={tmpdir / 'ir'}",
        ]

        def checked_output(inputs):
            result = run_analyzer_uncached(inputs + common)
            output = result.stdout or ""
            if not expect_returncode_zero(result, output + (result.stderr or ""), "wrapper metadata run failed"):
                return None
            if output.count("potential resource leak:") != 1 or "Function: actual_leak " not in output:
                fail_check("only actual_leak should report a leak; wrappers must preserve metadata", output)
                return None
            return output

        if checked_output([str(local), "--no-resource-cross-tu"]) is None:
            return False
        inputs = [str(definition), str(caller)]
        first = checked_output(inputs)
        if first is None or checked_output(inputs) != first:
            return fail_check("wrapper metadata differs after reading the summary cache")
        cache_files = list(cache.glob("*.json"))
        if not cache_files:
            return fail_check("wrapper metadata summary cache was not populated")
        summaries = [json.loads(path.read_text()) for path in cache_files]
        functions = {fn["name"]: fn["ownership"] for summary in summaries for fn in summary["functions"]}
        if not any(p["uncertainInputs"] & 2 for p in functions["unknown_wrapper"]["normal"]["params"]):
            return fail_check("cached wrapper lost Owned-input uncertainty")
        if not any(p["uncertainInputs"] & 2 for p in functions["unknown_slot_wrapper"]["normal"]["pointeeParams"]):
            return fail_check("cached wrapper lost pointee uncertainty")
        if functions["conditional_wrapper"]["normal"]["returns"] != "conditional":
            return fail_check("cached wrapper lost its conditional acquisition")

        # Old summaries have no uncertainty metadata and must be rebuilt, not reused.
        for path, summary in zip(cache_files, summaries):
            summary["schema"] = "resource-summary-cache-v4"
            for fn in summary["functions"]:
                for kind in ("normal", "exceptional"):
                    exit_summary = fn["ownership"][kind]
                    for param in exit_summary["params"] + exit_summary["pointeeParams"]:
                        param.pop("uncertainInputs", None)
            path.write_text(json.dumps(summary))
        if checked_output(inputs) != first:
            return fail_check("obsolete summaries changed the wrapper diagnostics")
        # Intermediate fixpoint keys need not be reused; active keys must be rewritten.
        if not any(json.loads(path.read_text())["schema"] == "resource-summary-cache-v5" for path in cache_files):
            return fail_check("obsolete ownership summaries were not rebuilt")
    print("  ✅ ownership wrapper metadata preserved locally, across TUs and in the cache\n")
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


def check_uninitialized_cross_tu_no_effect() -> bool:
    """
    #157: the summary of a function that writes nothing through its pointer parameters crosses
    files, and its callers report the reads that one file reports, whatever the sort order. It
    never comes from a weak definition, from a symbol that several files define, or from another
    ABI symbol with the same canonical name. An assembler name reaches the same symbol, and a
    static homonym hides nothing.
    """
    print("=== Testing uninitialized summaries that write nothing, across files ===")
    fixtures = RUN_CONFIG.test_dir / "uninitialized-variable"
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]
    name = "cross-tu-uninitialized-{}".format

    def reported(files: list[str], extra: list[str]) -> Optional[list[str]]:
        result = run_analyzer([*(str(fixtures / f) for f in files), "--format=json", *extra])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            return None
        return sorted(
            {
                str(d.get("location", {}).get("function", ""))
                for d in diagnostics
                if d.get("ruleId") == "UninitializedLocalRead"
            }
        )

    cases = []
    for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
        for label, parts, expected in (
            ("use alone", ["noeffect-use.c"], []),
            ("def and use", ["noeffect-def.c", "noeffect-use.c"], ["uses_peek"]),
            ("weak def", ["noeffect-weak.c", "noeffect-use.c"], []),
            (
                "defined twice, neither writes",
                ["noeffect-def.c", "noeffect-again.c", "noeffect-use.c"],
                [],
            ),
            (
                "defined twice, one writes",
                ["noeffect-def.c", "noeffect-writes.c", "noeffect-use.c"],
                [],
            ),
            (
                "static homonym",
                ["noeffect-def.c", "noeffect-static.c", "noeffect-use.c"],
                ["local_peek", "uses_peek"],
            ),
            ("assembler name", ["noeffect-def.c", "noeffect-asm-use.c"], ["uses_asm_name"]),
            ("other ABI symbol", ["abi-def.cpp", "abi-use.cpp"], []),
        ):
            cases.append((f"{pass_name}, {label}", [name(p) for p in parts], pass_args, expected))

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_uninitialized_no_effect_") as tmp:
        # The caller sorted before the definition: the analyzer sorts its inputs.
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            files = []
            for rank, part in enumerate(["noeffect-use.c", "noeffect-def.c"]):
                target = Path(tmp) / f"{rank}-{name(part)}"
                shutil.copy(fixtures / name(part), target)
                files.append(str(target))
            cases.append((f"{pass_name}, use sorted first", files, pass_args, ["uses_peek"]))

        for label, files, extra, expected in cases:
            found = reported(files, extra)
            if found == expected:
                print(f"  ✅ {label}: {found}")
            else:
                print(f"  ❌ {label}: {found}, expected {expected}")
                ok = False
    print()
    return ok


def check_const_param_cross_tu() -> bool:
    """
    #157: a pointer passed only to a function defined in another file, whose parameter points to
    const, could point to const, as within one file, whatever the sort order. The fact comes only
    from external definitions that are all exact and all declare it, and applies only to a call that
    passes one argument per parameter. An assembler name reaches the same symbol, a C++ overload is
    another symbol, and a static homonym gives nothing and hides nothing.
    """
    print("=== Testing const parameters of functions defined in another file ===")
    fixtures = RUN_CONFIG.test_dir / "pointer_reference-const_correctness"
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]
    name = "cross-tu-const-param-{}".format

    def reported(files: list[str], extra: list[str]) -> Optional[list[str]]:
        result = run_analyzer([*files, "--format=json", *extra])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            return None
        return sorted(
            {
                str(d.get("location", {}).get("function", ""))
                for d in diagnostics
                if str(d.get("ruleId", "")).startswith("ConstParameterNotModified")
            }
        )

    cases = []
    for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
        for label, parts, expected in (
            ("use alone", ["use.c"], []),
            ("def and use", ["def.c", "use.c"], ["uses_read_only"]),
            ("non-const definition", ["nonconst.c", "use.c"], ["read_only"]),
            ("defined twice, both const", ["def.c", "again.c", "use.c"], ["uses_read_only"]),
            ("defined twice, const and not const", ["def.c", "nonconst.c", "use.c"], ["read_only"]),
            ("weak def", ["weak.c", "use.c"], []),
            ("weak beside the definition", ["def.c", "weak.c", "use.c"], []),
            ("static homonym alone", ["static.c", "use.c"], []),
            (
                "static homonym beside the definition",
                ["def.c", "static.c", "use.c"],
                ["uses_read_only"],
            ),
            ("no prototype", ["def.c", "noproto-use.c"], ["uses_noproto"]),
            ("assembler name", ["def.c", "asm-use.c"], ["uses_asm_name"]),
            (
                "C++ overloads",
                ["overload-def.cpp", "overload-use.cpp"],
                ["_Z19uses_const_overloadPi"],
            ),
        ):
            files = [str(fixtures / name(p)) for p in parts]
            cases.append((f"{pass_name}, {label}", files, pass_args, expected))

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_const_param_") as tmp:
        # The analyzer sorts its inputs: these copies put the caller, then the non-const
        # definition, first.
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for label, parts, expected in (
                ("use sorted first", ["use.c", "def.c"], ["uses_read_only"]),
                ("non-const sorted first", ["nonconst.c", "def.c", "use.c"], ["read_only"]),
            ):
                files = []
                for rank, part in enumerate(parts):
                    target = Path(tmp) / f"{rank}-{name(part)}"
                    shutil.copy(fixtures / name(part), target)
                    files.append(str(target))
                cases.append((f"{pass_name}, {label}", files, pass_args, expected))

        for label, files, extra, expected in cases:
            found = reported(files, extra)
            if found == expected:
                print(f"  ✅ {label}: {found}")
            else:
                print(f"  ❌ {label}: {found}, expected {expected}")
                ok = False
    print()
    return ok


def _check_cross_tu_rule(
    title: str,
    directory: str,
    prefix: str,
    rule_id: str,
    cases: list[tuple[str, list[str], list[str]]],
    copied_cases: list[tuple[str, list[str], list[str]]],
) -> bool:
    """
    Runs each case in both passes: the analyzer over its fixtures, then the functions reported
    under rule_id. The analyzer sorts its inputs: copied_cases run on copies named so that the
    files keep the listed order.
    """
    print(f"=== Testing {title} ===")
    fixtures = RUN_CONFIG.test_dir / directory
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]

    def reported(files: list[str], extra: list[str]) -> Optional[list[str]]:
        result = run_analyzer([*files, "--format=json", *extra])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            return None
        return sorted(
            {
                str(d.get("location", {}).get("function", ""))
                for d in diagnostics
                if d.get("ruleId") == rule_id
            }
        )

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_rule_") as tmp:
        runs = []
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for label, parts, expected in cases:
                files = [str(fixtures / f"{prefix}{part}") for part in parts]
                runs.append((f"{pass_name}, {label}", files, pass_args, expected))
            for label, parts, expected in copied_cases:
                files = []
                for rank, part in enumerate(parts):
                    target = Path(tmp) / f"{rank}-{prefix}{part}"
                    shutil.copy(fixtures / f"{prefix}{part}", target)
                    files.append(str(target))
                runs.append((f"{pass_name}, {label}", files, pass_args, expected))

        for label, files, extra, expected in runs:
            found = reported(files, extra)
            if found == expected:
                print(f"  ✅ {label}: {found}")
            else:
                print(f"  ❌ {label}: {found}, expected {expected}")
                ok = False
    print()
    return ok


def check_duplicate_if_cross_tu() -> bool:
    """
    #157: a function defined in another file is deterministic when every definition of it is, as
    within one file, whatever the sort order: an else-if that repeats its if through it is
    reported. The fact follows a chain of files, never a cycle, and never comes from a definition
    that reads a mutable global, from a weak one or from a static homonym. A function that calls
    abort() stays deterministic, and a call through a declaration without a prototype qualifies.
    """
    both = ["pick", "pick_or_abort"]
    return _check_cross_tu_rule(
        "deterministic functions defined in another file",
        "diagnostics",
        "cross-tu-deterministic-",
        "DuplicateIfCondition",
        [
            ("use alone", ["use.c"], []),
            ("def and use", ["def.c", "use.c"], both),
            ("mutable global", ["global.c", "use.c"], []),
            ("defined twice, both deterministic", ["def.c", "again.c", "use.c"], both),
            (
                "defined twice, one reads a mutable global",
                ["def.c", "global.c", "use.c"],
                ["pick_or_abort"],
            ),
            ("weak def", ["weak.c", "use.c"], []),
            ("weak beside the definition", ["def.c", "weak.c", "use.c"], ["pick_or_abort"]),
            ("static homonym alone", ["static.c", "use.c"], ["local_pick"]),
            (
                "static homonym beside the definition",
                ["def.c", "static.c", "use.c"],
                ["local_pick", *both],
            ),
            (
                "chain over three files",
                ["chain-leaf.c", "chain-mid.c", "chain-use.c"],
                ["pick_chain"],
            ),
            ("chain without its leaf", ["chain-mid.c", "chain-use.c"], []),
            ("cycle across two files", ["cycle-a.c", "cycle-b.c"], []),
            ("no prototype", ["def.c", "noproto-use.c"], ["pick_noproto"]),
        ],
        [
            ("use sorted first", ["use.c", "def.c"], both),
            ("mutable global sorted first", ["global.c", "def.c", "use.c"], ["pick_or_abort"]),
            (
                "chain in reverse order",
                ["chain-use.c", "chain-mid.c", "chain-leaf.c"],
                ["pick_chain"],
            ),
        ],
    )


def check_size_minus_one_cross_tu() -> bool:
    """
    #157: the length that a function defined in another file passes to a bounded write reaches
    its callers, as within one file, whatever the sort order: a length n - 1 that may wrap is
    reported. The pair (destination, length) holds only if every definition has it, follows a
    chain of files, never comes from a weak definition or a static homonym, and applies to a call
    that passes one argument per parameter, also through a declaration without a prototype.
    """
    return _check_cross_tu_rule(
        "size-minus-one lengths through functions defined in another file",
        "size-arg",
        "cross-tu-size-minus-one-",
        "SizeMinusOneWrite",
        [
            ("use alone", ["use.c"], []),
            ("def and use", ["def.c", "use.c"], ["copy_name"]),
            ("defined twice, both pass n", ["def.c", "again.c", "use.c"], ["copy_name"]),
            ("defined twice, one does not pass n", ["def.c", "other.c", "use.c"], []),
            ("weak def", ["weak.c", "use.c"], []),
            ("weak beside the definition", ["def.c", "weak.c", "use.c"], []),
            ("static homonym alone", ["static.c", "use.c"], ["local_copy"]),
            (
                "static homonym beside the definition",
                ["def.c", "static.c", "use.c"],
                ["copy_name", "local_copy"],
            ),
            (
                "chain over three files",
                ["chain-leaf.c", "chain-mid.c", "chain-use.c"],
                ["copy_chain"],
            ),
            ("chain without its leaf", ["chain-mid.c", "chain-use.c"], []),
            ("no prototype", ["def.c", "noproto-use.c"], ["copy_noproto"]),
        ],
        [
            ("use sorted first", ["use.c", "def.c"], ["copy_name"]),
            ("other sorted first", ["other.c", "def.c", "use.c"], []),
            (
                "chain in reverse order",
                ["chain-use.c", "chain-mid.c", "chain-leaf.c"],
                ["copy_chain"],
            ),
        ],
    )


def check_cross_tu_call_graph() -> bool:
    """
    #157: a call to a declaration reaches the definition of its symbol in another file when that
    definition is exact and unique, and the files then conclude as one file does. A cycle across
    files is recursive, and never returns when no path leaves it. Its members and callers have an
    unknown max stack with a lower bound. A caller counts the frame of a callee defined in another
    file, in both modes and with --assume-external-frame. A static homonym, a symbol defined twice
    or a weak definition is never reached, and a call through a declaration without a prototype
    is. The sort order changes nothing.
    """
    print("=== Testing one call graph over the files analyzed together ===")
    fixtures = RUN_CONFIG.test_dir / "recursion/c"
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]

    def part(path: str) -> str:
        return Path(path).name.split("cross-tu-", 1)[-1].removesuffix(".c")

    def observed(files: list[str], extra: list[str]):
        result = run_analyzer([*files, "--format=json", *extra])
        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError:
            return None, None
        functions = {}
        for fn in payload.get("functions", []):
            known = fn.get("maxStackUnknown") is False
            functions[(part(str(fn.get("file", ""))), fn.get("name"))] = (
                "known" if known else "unknown",
                fn.get("maxStack") if known else fn.get("maxStackLowerBound"),
                bool(fn.get("isRecursive")),
            )
        recursion = sorted(
            (
                str(d.get("ruleId", "")).split(".", 1)[-1],
                part(str(d.get("location", {}).get("file", ""))),
                d.get("location", {}).get("function"),
            )
            for d in payload.get("diagnostics", [])
            if str(d.get("ruleId", "")).startswith("Recursion.")
        )
        return functions, recursion

    U, K = "unknown", "known"
    no_way_out = [
        ("Detected", "cycle-noexit-a", "ping"),
        ("Unconditional", "cycle-noexit-a", "ping"),
    ]
    exit_cycle = {
        ("cycle-exit-a", "ping"): (U, 32, True),
        ("cycle-exit-b", "pong"): (U, 32, True),
        ("cycle-exit-b", "enter_cycle"): (U, 48, False),
    }
    exit_recursion = [("Detected", "cycle-exit-a", "ping"), ("Detected", "cycle-exit-b", "pong")]
    static_pong = {
        ("cycle-static", "local_pong"): (K, 32, False),
        ("cycle-static", "pong"): (K, 16, False),
    }
    stack = {
        ("stack-frame", "big_frame"): (K, 4112, False),
        ("stack-caller", "big_caller"): (K, 4128, False),
    }
    unreached = {("stack-caller", "big_caller"): (U, 16, False)}

    # (label, fixtures, extra arguments, expected functions, expected Recursion diagnostics)
    cases = [
        (
            "caller alone",
            ["stack-caller"],
            [],
            {**unreached, ("stack-caller", "calls_external"): (U, None, False)},
            [],
        ),
        (
            "cycle without a way out",
            ["cycle-noexit-a", "cycle-noexit-b"],
            [],
            {
                ("cycle-noexit-a", "ping"): (U, 32, True),
                ("cycle-noexit-b", "pong"): (U, 32, True),
            },
            sorted(
                no_way_out
                + [
                    ("Detected", "cycle-noexit-b", "pong"),
                    ("Unconditional", "cycle-noexit-b", "pong"),
                ]
            ),
        ),
        (
            "cycle without a way out, no prototype",
            ["cycle-noexit-a", "cycle-noexit-noproto"],
            [],
            {
                ("cycle-noexit-a", "ping"): (U, 32, True),
                ("cycle-noexit-noproto", "pong"): (U, 32, True),
            },
            sorted(
                no_way_out
                + [
                    ("Detected", "cycle-noexit-noproto", "pong"),
                    ("Unconditional", "cycle-noexit-noproto", "pong"),
                ]
            ),
        ),
        ("cycle with a way out", ["cycle-exit-a", "cycle-exit-b"], [], exit_cycle, exit_recursion),
        (
            "static homonym alone",
            ["cycle-exit-a", "cycle-static"],
            [],
            {("cycle-exit-a", "ping"): (U, 16, False), **static_pong},
            [],
        ),
        (
            "static homonym beside the cycle",
            ["cycle-exit-a", "cycle-exit-b", "cycle-static"],
            [],
            {**exit_cycle, **static_pong},
            exit_recursion,
        ),
        (
            "stack",
            ["stack-frame", "stack-caller"],
            [],
            {**stack, ("stack-caller", "calls_external"): (U, None, False)},
            [],
        ),
        (
            "stack, ABI mode",
            ["stack-frame", "stack-caller"],
            ["--mode=abi"],
            {
                ("stack-frame", "big_frame"): (K, 4112, False),
                ("stack-caller", "big_caller"): (K, 4144, False),
                ("stack-caller", "calls_external"): (U, 16, False),
            },
            [],
        ),
        (
            "stack, external frame",
            ["stack-frame", "stack-caller"],
            ["--assume-external-frame=100"],
            {**stack, ("stack-caller", "calls_external"): (K, 100, False)},
            [],
        ),
        ("defined twice", ["stack-frame", "stack-frame-again", "stack-caller"], [], unreached, []),
        ("weak definition", ["stack-frame-weak", "stack-caller"], [], unreached, []),
        (
            "no prototype",
            ["stack-frame", "stack-noproto-caller"],
            [],
            {("stack-noproto-caller", "noproto_caller"): (K, 4128, False)},
            [],
        ),
    ]
    # The analyzer sorts its inputs: these run on copies named so that the files keep this order.
    copied_cases = [
        ("caller sorted first", ["stack-caller", "stack-frame"], [], stack, []),
        (
            "cycle sorted in reverse",
            ["cycle-exit-b", "cycle-exit-a"],
            [],
            exit_cycle,
            exit_recursion,
        ),
    ]

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_call_graph_") as tmp:
        runs = []
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for label, parts, extra, functions, recursion in cases:
                files = [str(fixtures / f"cross-tu-{p}.c") for p in parts]
                run = (f"{pass_name}, {label}", files, [*extra, *pass_args], functions, recursion)
                runs.append(run)
            for label, parts, extra, functions, recursion in copied_cases:
                files = []
                for rank, p in enumerate(parts):
                    target = Path(tmp) / f"{rank}-cross-tu-{p}.c"
                    shutil.copy(fixtures / f"cross-tu-{p}.c", target)
                    files.append(str(target))
                run = (f"{pass_name}, {label}", files, [*extra, *pass_args], functions, recursion)
                runs.append(run)

        for label, files, extra, functions, recursion in runs:
            found_functions, found_recursion = observed(files, extra)
            if found_functions is None:
                print(f"  ❌ {label}: no JSON output")
                ok = False
                continue
            wrong = {
                key: (found_functions.get(key), value)
                for key, value in functions.items()
                if found_functions.get(key) != value
            }
            if not wrong and found_recursion == recursion:
                print(f"  ✅ {label}")
                continue
            ok = False
            print(f"  ❌ {label}")
            for key, (got, expected) in wrong.items():
                print(f"     {key}: {got}, expected {expected}")
            if found_recursion != recursion:
                print(f"     recursion: {found_recursion}, expected {recursion}")
    print()
    return ok


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


def check_noreturn_cross_tu() -> bool:
    """
    #153: a call to a function that never returns ends the path, also when the function is
    defined in another file analyzed alongside, or at the end of a chain over three files,
    whatever the order of the files and --jobs. A static function is never matched by name with
    a function of another file.

    #170: a symbol never returns only if each of its definitions is exact and never returns,
    whatever their sort order. A definition that returns, or a weak one, keeps the guards of
    fail()'s callers from protecting anything, down to the callers of must_fail(). A static
    homonym is not a definition of the symbol.
    """
    print("=== Testing functions that never return across files ===")
    fixtures = RUN_CONFIG.test_dir / "noreturn"
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]
    watched = {"SizeMinusOneWrite", "IntegerOverflow.SignedArithmetic", "UninitializedLocalRead"}

    def reported(files: list[str], extra: list[str]) -> Optional[list[str]]:
        result = run_analyzer([*(str(fixtures / name) for name in files), "--format=json", *extra])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            return None
        return sorted(
            {
                str(d.get("location", {}).get("function", ""))
                for d in diagnostics
                if d.get("ruleId") in watched
            }
        )

    pair = ["cross-tu-noreturn-def.c", "cross-tu-noreturn-use.c"]
    chain = [
        "cross-tu-noreturn-chain-fail.c",
        "cross-tu-noreturn-chain-twice.c",
        "cross-tu-noreturn-chain-use.c",
    ]
    statics = ["cross-tu-noreturn-static-a.c", "cross-tu-noreturn-static-b.c"]
    conflict = "cross-tu-noreturn-conflict-{}.c".format
    both = ["copy", "copy_twice_removed"]
    cases = []
    for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
        # pick's guard relates i to n: only the solver uses it.
        kept = ["copy_elsewhere", "copy_maybe"] + (["pick"] if pass_name == "default" else [])
        for order, files in (("def, use", pair), ("use, def", pair[::-1])):
            for jobs in ("--jobs=1", "--jobs=2"):
                cases.append((f"{pass_name}, {order}, {jobs}", files, [*pass_args, jobs], kept))
        cases.append((f"{pass_name}, chain over three files", chain, pass_args, []))
        cases.append((f"{pass_name}, static homonyms", statics, pass_args, ["copy_b"]))
        callers = [conflict("use"), conflict("indirect")]
        for label, parts, expected in (
            ("fail() only defined to exit", ["exits"], []),
            ("fail() only defined to return", ["returns"], both),
            ("fail() also defined weak", ["exits", "weak"], both),
            ("fail() also defined static, to return", ["exits", "static"], []),
        ):
            files = [*map(conflict, parts), *callers]
            cases.append((f"{pass_name}, {label}", files, pass_args, expected))

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_noreturn_conflict_") as tmp:
        # The two definitions of fail(), sorted either way: the analyzer sorts its inputs.
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for first, second in (("exits", "returns"), ("returns", "exits")):
                renamed = Path(tmp) / f"{first}-first"
                renamed.mkdir(exist_ok=True)
                files = []
                for rank, part in enumerate([first, second, "use", "indirect"]):
                    target = renamed / f"{rank}-{conflict(part)}"
                    shutil.copy(fixtures / conflict(part), target)
                    files.append(str(target))
                label = f"{pass_name}, fail() defined twice, the one that {first} sorted first"
                cases.append((label, files, pass_args, both))

        for label, files, extra, expected in cases:
            found = reported(files, extra)
            if found == expected:
                print(f"  ✅ {label}: {found}")
            else:
                print(f"  ❌ {label}: {found}, expected {expected}")
                ok = False
    print()
    return ok


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
    with tempfile.TemporaryDirectory(prefix="ct_uaf_cross_tu_") as tmp:
        tmpdir = Path(tmp)
        resource_cache_arg = f"--resource-summary-cache-dir={tmpdir / 'resource-cache'}"
        compile_cache_arg = f"--compile-ir-cache-dir={tmpdir / 'compile-ir-cache'}"

        result = run_analyzer(
            [
                str(def_file),
                str(use_file),
                "--jobs=2",
                "--analysis-profile=full",
                "--resource-cross-tu",
                f"--resource-model={model}",
                resource_cache_arg,
                compile_cache_arg,
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
                resource_cache_arg,
                compile_cache_arg,
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


def check_diagnostic_cwe_coverage() -> bool:
    """
    Every diagnostic names its rule, and each kind carries the CWE of the weakness it
    detects, so code-scanning platforms can classify it.
    """
    print("=== Testing diagnostic CWE coverage ===")
    ok = True

    # (rule, CWE, analyzer args): one fixture per kind and per CWE the kind can carry.
    cases = [
        ("StackBufferOverflow", "CWE-121", ["test/bound-storage/bound-storage.c"]),
        ("StackBufferOverflow", "CWE-787", ["test/bound-storage/global-array-overflow.c"]),
        ("StackBufferOverflow", "CWE-125", ["test/bound-storage/read-access-out-of-bounds.c"]),
        ("NegativeStackIndex", "CWE-124", ["test/bound-storage/ranges_test.c"]),
        ("NegativeStackIndex", "CWE-127", ["test/bound-storage/read-access-out-of-bounds.c"]),
        (
            "MemcpyWithStackDest",
            "CWE-120",
            [
                "test/cpy-buffer/unbounded-strcpy-model.c",
                "--buffer-model=models/buffer-overflow/generic.txt",
            ],
        ),
        ("MemcpyWithStackDest", "CWE-121", ["test/cpy-buffer/bad-usage-memcpy.c"]),
        ("VLAUsage", "CWE-770", ["test/vla/vla-unknown-stack.c"]),
        ("AllocaTooLarge", "CWE-770", ["test/alloca/oversized-constant.c"]),
        ("AllocaUsageWarning", "CWE-770", ["test/vla/deguised-constant.c"]),
        ("AllocaUserControlled", "CWE-789", ["test/alloca/user-controlled.c"]),
        ("SizeMinusOneWrite", "CWE-191", ["test/size-arg/strncpy-size-minus-1.c"]),
        (
            "InvalidBaseReconstruction",
            "CWE-823",
            ["test/offset_of-container_of/container_of_wrong_member_offset_error.c"],
        ),
        ("StackPointerEscape", "CWE-562", ["test/escape-stack/return-buf.c"]),
        ("DuplicateIfCondition", "CWE-561", ["test/diagnostics/duplicate-else-if-basic.c"]),
        # Also reports Recursion.Detected, the informational diagnostic, which names no CWE:
        # the check below still requires it to carry a rule.
        ("Recursion.Unconditional", "CWE-674", ["test/recursion/c/infinite-recursion.c"]),
    ]

    for rule, cwe, args in cases:
        label = f"{rule} {cwe}"
        result = run_analyzer([*args, "--format=json"])
        if result.returncode != 0:
            print(f"  ❌ {label} run failed (code {result.returncode})")
            print((result.stdout or "") + (result.stderr or ""))
            ok = False
            continue

        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError as exc:
            print(f"  ❌ {label} invalid JSON output: {exc}")
            print(result.stdout or "")
            ok = False
            continue

        unnamed = [d for d in diagnostics if d.get("ruleId") in ("", "None")]
        if unnamed:
            print(f"  ❌ {label} {args[0]} has {len(unnamed)} diagnostic(s) under rule None")
            ok = False
            continue

        cwes = {d.get("cwe") for d in diagnostics if d.get("ruleId") == rule}
        if cwes != {cwe}:
            print(f"  ❌ {label} {args[0]}: {rule} diagnostics carry {json.dumps(sorted(cwes, key=str))}")
            ok = False
            continue

        print(f"  ✅ {label} OK")

    print()
    return ok


def check_diagnostic_paths_follow_the_input() -> bool:
    """
    Every diagnostic and function names its file as the input was given, whether the rule
    locates it through debug info (signed overflow: an absolute path) or not (an uninitialized
    read: the input name). A relative input stays relative, an absolute one absolute.
    """
    print("=== Testing diagnostic paths follow the input ===")
    relative = "test/integer-overflow/cross-tu-tricky-def.c"
    ok = True
    for label, given in (("relative input", relative), ("absolute input", str(Path(relative).resolve()))):
        result = run_analyzer([given, "--format=json"])
        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError:
            ok = fail_check(f"{label}: no JSON output", (result.stdout or "") + (result.stderr or ""))
            continue
        files = {d.get("location", {}).get("file", "") for d in payload.get("diagnostics", [])}
        files |= {f.get("file", "") for f in payload.get("functions", [])}
        rules = {d.get("ruleId") for d in payload.get("diagnostics", [])}
        if files != {given} or "IntegerOverflow.SignedArithmetic" not in rules:
            ok = fail_check(f"{label}: expected every file to be {given!r}, got {sorted(files)}")
            continue
        print(f"  ✅ {label}: {given}")
    print()
    return ok


def check_calls_through_aliases() -> bool:
    """
    #179: on ELF targets, clang calls a constructor or a destructor defined out of its class
    through an alias (C1 -> C2, D1 -> D2), a direct call that Mach-O makes without one. The
    findings, and whether each max stack is known, must be the same for x86-64 and AArch64 Linux
    as for macOS: an object that only reaches its constructor does not escape, the call does not
    hide that the constructor initializes it, and a constructor that keeps this is reported. A
    constructor defined in another file must resolve through its alias too. The max stacks
    themselves may differ: Mach-O keeps C1 as a function that calls C2, a frame that ELF folds
    into the alias.
    """
    print("=== Testing calls through constructor and destructor aliases, per target ===")
    targets = ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu", "arm64-apple-macosx")
    smt_args = [
        "--smt=on",
        "--smt-backend=z3",
        "--smt-mode=single",
        f"--smt-rules={','.join(_all_smt_rules())}",
        f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
    ]

    def observed(sources: list[Path], target: str, extra: list[str]):
        result = run_analyzer(
            [*map(str, sources), "--format=json", f"--compile-arg=--target={target}", *extra]
        )
        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError:
            return None
        # Constructors and destructors are left out: Mach-O lists C1 and D1, ELF only aliases.
        stacks = {
            fn.get("name"): "known" if fn.get("maxStackUnknown") is False else "unknown"
            for fn in payload.get("functions", [])
            if not str(fn.get("name", "")).startswith("_ZN")
        }
        findings = sorted(
            {
                (str(d.get("ruleId", "")).split(".", 1)[0], d.get("location", {}).get("function"))
                for d in payload.get("diagnostics", [])
            }
        )
        return stacks, findings

    with tempfile.TemporaryDirectory(prefix="ct_alias_cross_file_") as tmp:
        cross = Path(tmp)
        (cross / "resolver.hpp").write_text(
            "struct Resolver\n{\n    explicit Resolver(int value);\n    int seed;\n};\n"
        )
        (cross / "resolver.cpp").write_text(
            '#include "resolver.hpp"\n\nResolver::Resolver(int value) : seed(value) {}\n'
        )
        (cross / "run.cpp").write_text(
            '#include "resolver.hpp"\n\nint run(int value)\n{\n'
            "    const Resolver resolver(value);\n    return resolver.seed;\n}\n"
        )
        escape = "StackPointerEscape"
        # (label, sources, extra arguments, functions whose max stack must be known, findings
        # expected on every target).
        cases = [
            ("escape-stack/ctor-alias-no-escape.cpp", None, [], ["_Z3runi"], []),
            (
                "false-positive-repro/ctor-dtor-alias.cpp",
                None,
                [],
                ["_Z3usev", "_Z5framev"],
                [("UninitializedLocalRead", "_Z3usev")],
            ),
            ("escape-stack/ctor-alias-stores-this.cpp", None, [], ["_Z3runi"], [(escape, "_Z3runi")]),
            (
                "escape-stack/ctor-alias-passes-this.cpp",
                None,
                [],
                [],
                [(escape, "_Z3runiPFvP8ResolverE")],
            ),
            (
                "constructor defined in another file",
                [cross / "resolver.cpp", cross / "run.cpp"],
                [f"--compile-arg=-I{cross}"],
                ["_Z3runi"],
                [],
            ),
        ]
        ok = True
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for name, sources, case_args, known_stacks, expected in cases:
                label = f"{pass_name}, {name}"
                files = sources or [RUN_CONFIG.test_dir / name]
                runs = {t: observed(files, t, [*case_args, *pass_args]) for t in targets}
                problems = []
                if any(run is None for run in runs.values()):
                    missing = sorted(t for t, r in runs.items() if r is None)
                    problems.append(f"no JSON output: {missing}")
                else:
                    first = runs[targets[0]]
                    for target in targets[1:]:
                        if runs[target] != first:
                            problems.append(f"{target}: {runs[target]}, {targets[0]}: {first}")
                    for target, (stacks, findings) in runs.items():
                        unknown = [f for f in known_stacks if stacks.get(f) != "known"]
                        if unknown:
                            problems.append(f"{target}: max stack not known for {unknown}")
                        if findings != expected:
                            problems.append(f"{target}: findings {findings}, expected {expected}")
                if problems:
                    ok = False
                    print(f"  ❌ {label}")
                    for problem in problems:
                        print(f"     {problem}")
                else:
                    print(f"  ✅ {label}: {runs[targets[0]][1]}")

    # Aliases the source declares, which clang supports on ELF targets only. An alias the link
    # may replace (weak) does not designate its aliasee for sure: the caller's max stack stays
    # unknown, whether or not the definition that replaces it is analyzed too. A plain alias does.
    with tempfile.TemporaryDirectory(prefix="ct_alias_attribute_") as tmp:
        root = Path(tmp)
        (root / "weak-alias.c").write_text(
            "int fallback(int n) { return n; }\n"
            'int replaceable(int n) __attribute__((weak, alias("fallback")));\n'
            "int caller(int n) { return replaceable(n); }\n"
        )
        (root / "plain-alias.c").write_text(
            "int fallback(int n) { return n; }\n"
            'int replaceable(int n) __attribute__((alias("fallback")));\n'
            "int caller(int n) { return replaceable(n); }\n"
        )
        (root / "override.c").write_text(
            "int replaceable(int n)\n{\n    volatile unsigned char buffer[256];\n"
            "    buffer[0] = (unsigned char)n;\n    return buffer[0];\n}\n"
        )
        alias_cases = [
            ("weak alias and the definition that replaces it", ["weak-alias.c", "override.c"], "unknown"),
            ("weak alias alone", ["weak-alias.c"], "unknown"),
            ("plain alias", ["plain-alias.c"], "known"),
        ]
        for pass_name, pass_args in (("default", []), ("smt-z3", smt_args)):
            for name, files, status in alias_cases:
                for target in targets[:2]:
                    run = observed([root / f for f in files], target, pass_args)
                    got = run[0].get("caller") if run else "no JSON output"
                    if got == status:
                        print(f"  ✅ {pass_name}, {name}, {target}: caller max stack {got}")
                    else:
                        ok = False
                        print(f"  ❌ {pass_name}, {name}, {target}: caller max stack {got}, expected {status}")
    print()
    return ok


def check_resource_cache_rebuilds_alias_summaries() -> bool:
    """
    Following a call through an alias changes the resource summaries (#179): a wrapper that
    returns what an alias of an acquiring function returns now acquires. Summaries cached by a
    previous version of the analyzer, under the previous cache schema, must be rebuilt: the
    summaries a run with that cache writes must be those a run with an empty cache writes, the
    wrapper's acquisition included, not only the same diagnostics. Entries for intermediate
    rounds of the fixed point that a run does not read again may stay as they were. The cache
    keys hash the IR as printed, which differs between a fresh compile and the compile IR cache:
    a first run fills that cache, so that both runs compared take their IR from it.
    """
    print("=== Testing that resource summaries cached by a previous schema are rebuilt ===")
    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_alias_resource_cache_") as tmp:
        root = Path(tmp)
        (root / "alias.c").write_text(
            "static int storage;\n"
            "void *create_impl(void) { return &storage; }\n"
            'void *alias_create(void) __attribute__((alias("create_impl")));\n'
            "void *wrapper(void) { return alias_create(); }\n"
        )
        (root / "caller.c").write_text(
            "void *wrapper(void);\nvoid release_handle(void *);\n"
            "void consumer(void) { release_handle(wrapper()); }\n"
        )
        model = root / "model.txt"
        model.write_text("acquire_ret create_impl Handle\nrelease_arg release_handle 0 Handle\n")

        def acquires(summary: dict) -> bool:
            return any(
                e.get("action") == "acquire_ret"
                for fn in summary["functions"] if fn["name"] == "wrapper" for e in fn["effects"]
            )

        for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"):
            cold, stale = root / f"cold-{target}", root / f"stale-{target}"

            def run(cache: Path):
                return run_analyzer_uncached(
                    [str(root / "alias.c"), str(root / "caller.c"), "--format=json", "--jobs=1",
                     f"--compile-arg=--target={target}", f"--resource-model={model}",
                     f"--compile-ir-cache-dir={root / f'ir-{target}'}",
                     f"--resource-summary-cache-dir={cache}"]
                )

            run(root / f"warm-up-{target}")
            first = run(cold)
            expected = {path.name: json.loads(path.read_text()) for path in cold.glob("*.json")}
            if first.returncode != 0 or not any(acquires(s) for s in expected.values()):
                ok = False
                print(f"  ❌ {target}: the wrapper's summary does not acquire with an empty cache")
                continue
            # The cache a previous version left: the previous schema, and a wrapper that does
            # not acquire, as it was summarized without following the alias.
            stale.mkdir()
            for path in cold.glob("*.json"):
                summary = json.loads(path.read_text())
                summary["schema"] = "resource-summary-cache-v4"
                for fn in summary["functions"]:
                    if fn["name"] == "wrapper":
                        fn["effects"] = []
                        fn["ownership"]["normal"]["returns"] = "unknown"
                        fn["ownership"]["normal"]["returnsKind"] = ""
                (stale / path.name).write_text(json.dumps(summary))
            second = run(stale)
            current = next(iter(expected.values()))["schema"]
            written = {
                path.name: summary
                for path in stale.glob("*.json")
                if (summary := json.loads(path.read_text()))["schema"] == current
            }
            if second.returncode != 0 or second.stdout != first.stdout:
                ok = False
                print(f"  ❌ {target}: the output with the previous cache differs from the one with an empty cache")
            elif any(summary != expected.get(name) for name, summary in written.items()) or not any(
                acquires(s) for s in written.values()
            ):
                ok = False
                print(f"  ❌ {target}: the summaries written with the previous cache are not those of an empty one: {written}")
            else:
                print(f"  ✅ {target}: the previous cache is rebuilt to the summaries of an empty one")
    print()
    return ok


def check_const_param_abi_split_struct() -> bool:
    """
    ConstParameterNotModified names the same source parameters whatever the ABI does with a
    struct taken by value: x86-64 System V passes a 16-byte struct as two IR parameters
    (name.coerce0, name.coerce1), AArch64 as one. Both targets must match what the source says.
    """
    print("=== Testing const-parameter findings across a split by-value struct ===")
    source = RUN_CONFIG.test_dir / "pointer_reference-const_correctness/abi-split-parameter.cpp"
    expected = {("p", "read_only_pointer"), ("p", "in_lambda")}
    ok = True
    for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"):
        result = run_analyzer([str(source), "--format=json", f"--compile-arg=--target={target}"])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            print(f"  ❌ {target}: no JSON output")
            ok = False
            continue
        found = set()
        for d in diagnostics:
            if not str(d.get("ruleId", "")).startswith("ConstParameterNotModified."):
                continue
            message = d.get("details", {}).get("message", "")
            match = re.search(r"parameter '([^']+)' in function '([A-Za-z_]\w*)", message)
            if match:
                found.add((match.group(1), match.group(2)))
        if found == expected:
            print(f"  ✅ {target}: {sorted(found)}")
        else:
            print(f"  ❌ {target}: {sorted(found)}, expected {sorted(expected)}")
            ok = False
    print()
    return ok


def check_escape_through_returned_value() -> bool:
    """
    StackPointerEscape reports the address of a local returned inside a struct or an integer on
    every target, whatever form the ABI gives the returned value: x86-64 returns a one-pointer
    struct as a pointer and a two-field struct as an aggregate, AArch64 and Apple arm64 as a
    converted integer and an array of integers.
    """
    print("=== Testing escapes through a returned value across targets ===")
    source = RUN_CONFIG.test_dir / "escape-stack/address-returned-in-value.c"
    expected = {
        "return_one_pointer_struct",
        "return_second_of_two_pointers",
        "return_after_int_field",
        "return_address_as_integer",
        "return_after_integer_round_trip",
    }
    ok = True
    for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu", "arm64-apple-macosx"):
        result = run_analyzer([str(source), "--format=json", f"--compile-arg=--target={target}"])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            print(f"  ❌ {target}: no JSON output")
            ok = False
            continue
        found = {
            d.get("location", {}).get("function")
            for d in diagnostics
            if d.get("ruleId") == "StackPointerEscape"
        }
        if found == expected:
            print(f"  ✅ {target}: {sorted(found)}")
        else:
            print(f"  ❌ {target}: {sorted(found)}, expected {sorted(expected)}")
            ok = False
    print()
    return ok


def check_uninitialized_receiver_abi_split_struct() -> bool:
    """
    A method call marks its receiver as constructed whatever the ABI does with a struct taken by
    value: x86-64 System V passes a 16-byte struct as two IR parameters after `this`, AArch64 as
    one. Neither target may report the receiver as never initialized.
    """
    print("=== Testing the method receiver across a split by-value struct ===")
    source = RUN_CONFIG.test_dir / "uninitialized-variable/abi-split-method-receiver.cpp"
    ok = True
    for target in ("x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu"):
        result = run_analyzer([str(source), "--format=json", f"--compile-arg=--target={target}"])
        try:
            diagnostics = json.loads(result.stdout or "").get("diagnostics", [])
        except json.JSONDecodeError:
            print(f"  ❌ {target}: no JSON output")
            ok = False
            continue
        reported = sorted(
            {
                str(d.get("location", {}).get("function", ""))
                for d in diagnostics
                if str(d.get("ruleId", "")).startswith("UninitializedLocal")
            }
        )
        if not reported:
            print(f"  ✅ {target}: no uninitialized-local finding")
        else:
            print(f"  ❌ {target}: uninitialized-local findings in {reported}")
            ok = False
    print()
    return ok


def check_sarif_rule_cwe_tags() -> bool:
    """
    A SARIF rule lists every CWE of its diagnostics in the run, whatever the order of the
    inputs: here StackBufferOverflow reports a write (CWE-121) and a read (CWE-125).
    """
    print("=== Testing SARIF rule CWE tags ===")
    write = "char write_past_end(int i) { char buf[10] = {0}; if (i <= 10) buf[i] = 'x'; return buf[0]; }\n"
    read = "char read_past_end(int i) { char buf[10] = {0}; if (i <= 10) return buf[i]; return 0; }\n"
    expected = ["external/cwe/cwe-121", "external/cwe/cwe-125"]
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for label, source in (("write first", write + read), ("read first", read + write)):
            path = Path(tmp) / f"{label.replace(' ', '-')}.c"
            path.write_text(source)
            result = run_analyzer([str(path), "--format=sarif"])
            try:
                rules = json.loads(result.stdout or "")["runs"][0]["tool"]["driver"]["rules"]
            except (ValueError, KeyError, IndexError):
                print(f"  ❌ {label}: no SARIF rules in the output")
                ok = False
                continue
            tags = next(
                (r.get("properties", {}).get("tags", []) for r in rules if r.get("id") == "StackBufferOverflow"),
                None,
            )
            # Only the CWE tags: a rule may carry others (the security tag).
            cwe_tags = None if tags is None else [t for t in tags if t.startswith("external/cwe/")]
            if cwe_tags != expected:
                print(f"  ❌ {label}: StackBufferOverflow CWE tags {cwe_tags}, expected {expected}")
                ok = False
    if ok:
        print("  ✅ SARIF rule CWE tags OK")
    print()
    return ok


def check_sarif_security_severity() -> bool:
    """
    GitHub code scanning ranks the alerts of a rule by its security-severity when the rule
    has the security tag: a stack buffer overflow is a security rule, a const hint is not.
    """
    print("=== Testing SARIF security severity ===")
    source = (
        "char write_past_end(int i) { char buf[10] = {0}; if (i <= 10) buf[i] = 'x'; return buf[0]; }\n"
        "int read_only(int *p) { return *p; }\n"
    )
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "security-severity.c"
        path.write_text(source)
        result = run_analyzer([str(path), "--format=sarif"])
    try:
        rules = json.loads(result.stdout or "")["runs"][0]["tool"]["driver"]["rules"]
    except (ValueError, KeyError, IndexError):
        print("  ❌ no SARIF rules in the output")
        print()
        return False

    properties = {r.get("id"): r.get("properties", {}) for r in rules}
    ok = True
    overflow = properties.get("StackBufferOverflow")
    if (
        overflow is None
        or "security" not in overflow.get("tags", [])
        or overflow.get("security-severity") != "9.3"
    ):
        print(f"  ❌ StackBufferOverflow properties {overflow}, expected the security tag and 9.3")
        ok = False
    hints = {rid: p for rid, p in properties.items() if rid.startswith("ConstParameterNotModified.")}
    if not hints:
        print("  ❌ no ConstParameterNotModified rule in the output")
        ok = False
    for rid, p in hints.items():
        if "security" in p.get("tags", []) or "security-severity" in p:
            print(f"  ❌ {rid} properties {p}, expected no security tag and no score")
            ok = False
    if ok:
        print("  ✅ SARIF security severity OK")
    print()
    return ok


CI_SCRIPT = Path(__file__).resolve().parent / "scripts" / "ci" / "run_code_analysis.py"


def _run_ci_script(args: list[str], jobs: int) -> subprocess.CompletedProcess:
    """Run scripts/ci/run_code_analysis.py with ANALYZER_JOBS=<jobs>."""
    return subprocess.run(
        [sys.executable, str(CI_SCRIPT), *args],
        capture_output=True,
        text=True,
        env={**os.environ, "ANALYZER_JOBS": str(jobs)},
        timeout=RUN_CONFIG.analyzer_timeout,
    )


def check_ci_script_sarif_merge() -> bool:
    """
    run_code_analysis.py runs the analyzer in chunks and merges their SARIF logs. The merged
    log must describe the same rules as one analyzer run over all the inputs: here a stack
    write in one file, and a stack read and a read-only pointer parameter in the other.
    """
    print("=== Testing CI script SARIF merge ===")
    sources = {
        "write.c": "char write_past_end(int i) { char buf[10] = {0}; if (i <= 10) buf[i] = 'x'; return buf[0]; }\n",
        "read.c": "char read_past_end(int i) { char buf[10] = {0}; if (i <= 10) return buf[i]; return 0; }\n"
        "int read_only(int *p) { return *p; }\n",
    }

    def rules_of(log: dict) -> dict:
        # Tags compared as sets: their order carries no meaning.
        rules = log["runs"][0]["tool"]["driver"]["rules"]
        return {
            r["id"]: {**r, "properties": {**r.get("properties", {}), "tags": sorted(r.get("properties", {}).get("tags", []))}}
            for r in rules
        }

    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for name, text in sources.items():
            path = Path(tmp) / name
            path.write_text(text)
            paths.append(str(path))
        merged_path = Path(tmp) / "merged.sarif"
        script = _run_ci_script(
            ["--analyzer", str(RUN_CONFIG.analyzer), "--sarif-out", str(merged_path), "--fail-on", "none", *paths],
            jobs=2,
        )
        single = subprocess.run(
            [str(RUN_CONFIG.analyzer), *paths, "--format=sarif"],
            capture_output=True,
            text=True,
            timeout=RUN_CONFIG.analyzer_timeout,
        )
        try:
            merged = json.loads(merged_path.read_text())
            expected = json.loads(single.stdout or "")
        except (OSError, ValueError) as exc:
            print(f"  ❌ no SARIF log to compare: {exc}")
            print((script.stdout or "") + (script.stderr or ""))
            print()
            return False

    ok = True
    merged_rules, expected_rules = rules_of(merged), rules_of(expected)
    for rid in sorted(set(merged_rules) | set(expected_rules)):
        if merged_rules.get(rid) != expected_rules.get(rid):
            print(f"  ❌ rule {rid}: merged {merged_rules.get(rid)}, one run {expected_rules.get(rid)}")
            ok = False
    merged_results = merged["runs"][0].get("results", [])
    undescribed = sorted({r.get("ruleId") for r in merged_results} - set(merged_rules))
    if undescribed:
        print(f"  ❌ results without a rule: {undescribed}")
        ok = False
    if len(merged_results) != len(expected["runs"][0].get("results", [])):
        print(f"  ❌ {len(merged_results)} merged results, one run has {len(expected['runs'][0].get('results', []))}")
        ok = False
    if ok:
        print("  ✅ CI script SARIF merge OK")
    print()
    return ok


def check_ci_script_sarif_rule_join() -> bool:
    """
    When the chunk logs describe one rule differently, the merged rule has the tags of all of
    them and the highest security-severity, even when the first gives it neither (its
    diagnostics there carry no CWE).
    """
    print("=== Testing CI script SARIF rule join ===")
    spec = importlib.util.spec_from_file_location("run_code_analysis", CI_SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    merge_sarif_logs = getattr(module, "merge_sarif_logs", None)
    if merge_sarif_logs is None:
        print(f"  ❌ {CI_SCRIPT.name} has no merge_sarif_logs")
        print()
        return False

    def log(properties: Optional[dict]) -> dict:
        rule = {"id": "Shared"}
        if properties:
            rule["properties"] = properties
        run = {"tool": {"driver": {"name": "stand-in", "rules": [rule]}}, "results": []}
        return {"version": "2.1.0", "runs": [run]}

    # The highest score is neither the first log's nor the last's.
    logs = [
        log(None),
        log({"tags": ["security", "external/cwe/cwe-457"], "security-severity": "7.8"}),
        log({"tags": ["security", "external/cwe/cwe-200"], "security-severity": "6.5"}),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for i, content in enumerate(logs):
            path = Path(tmp) / f"chunk{i}.sarif"
            path.write_text(json.dumps(content))
            paths.append(str(path))
        merged = merge_sarif_logs(paths)

    rules = merged["runs"][0]["tool"]["driver"]["rules"]
    properties = rules[0].get("properties", {}) if len(rules) == 1 else {}
    tags = set(properties.get("tags", []))
    expected_tags = {"security", "external/cwe/cwe-457", "external/cwe/cwe-200"}
    ok = tags == expected_tags and properties.get("security-severity") == "7.8"
    if ok:
        print("  ✅ CI script SARIF rule join OK")
    else:
        print(f"  ❌ merged rules {rules}, expected one rule with tags {sorted(expected_tags)} and 7.8")
    print()
    return ok


def check_ci_script_sarif_chunks() -> bool:
    """
    run_code_analysis.py merges the chunk logs in input order, whatever chunk finishes first,
    and fails when a chunk log cannot be read. A stand-in analyzer controls both: its log has
    one rule named after its input, it sleeps first for slow.c and writes no JSON for broken.c.
    """
    print("=== Testing CI script SARIF chunks ===")
    stand_in = textwrap.dedent(
        """\
        #!/usr/bin/env python3
        import json, sys, time
        from pathlib import Path

        name = Path(next(a for a in sys.argv[1:] if not a.startswith("--"))).stem
        out = next(a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--sarif-out="))
        if name == "slow":
            time.sleep(1)
        run = {"tool": {"driver": {"name": "stand-in", "rules": [{"id": name}]}},
               "results": [{"ruleId": name, "message": {"text": name}}]}
        log = {"version": "2.1.0", "runs": [run]}
        Path(out).write_text("not json" if name == "broken" else json.dumps(log))
        print(json.dumps({"diagnostics": []}))
        """
    )
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        analyzer = Path(tmp) / "stand-in-analyzer"
        analyzer.write_text(stand_in)
        analyzer.chmod(0o755)
        for name in ("slow", "fast", "broken"):
            (Path(tmp) / f"{name}.c").write_text("int f(void) { return 0; }\n")
        out = Path(tmp) / "merged.sarif"

        def run_script(*names: str) -> subprocess.CompletedProcess:
            inputs = [str(Path(tmp) / f"{name}.c") for name in names]
            args = ["--analyzer", str(analyzer), "--sarif-out", str(out), "--fail-on", "none", *inputs]
            return _run_ci_script(args, jobs=len(inputs))

        # The first chunk finishes last: the merged results still follow the input order.
        result = run_script("slow", "fast")
        order = None
        if result.returncode == 0 and out.exists():
            order = [r.get("ruleId") for r in json.loads(out.read_text())["runs"][0].get("results", [])]
        if order != ["slow", "fast"]:
            print(f"  ❌ merged results in the order {order}, expected the input order ['slow', 'fast']")
            print((result.stdout or "") + (result.stderr or ""))
            ok = False

        # A chunk log that cannot be read fails the run instead of losing its results.
        result = run_script("fast", "broken")
        if result.returncode != 2:
            print(f"  ❌ an unreadable chunk log exits with code {result.returncode}, expected 2")
            ok = False
    if ok:
        print("  ✅ CI script SARIF chunks OK")
    print()
    return ok


COMMIT_CHECKER = Path(__file__).resolve().parent / "scripts" / "ci" / "commit_checker.py"
COMMIT_CHECK_WORKFLOW = Path(__file__).resolve().parent / ".github" / "workflows" / "commit-check.yml"


def check_ci_commit_checker_range() -> bool:
    """
    The Commit conventions workflow must check the commits of the pushed branch. actions/checkout
    makes origin/<branch> the upstream of the branch it checks out, so commit_checker.py, which
    falls back to the upstream, would diff HEAD against itself and check nothing (#145). The
    check replays that checkout in a scratch repository and runs the checker with the
    workflow's BASE_BRANCH: a branch with bad subjects must fail and list them, a branch of
    conventional, revert and merge commits must pass after checking a non-empty range.
    """
    print("=== Testing CI commit checker range ===")
    step = COMMIT_CHECK_WORKFLOW.read_text().split("- name: Validate commit messages", 1)[-1]
    base = re.search(r"^\s+BASE_BRANCH: (\S+)\s*$", step, re.MULTILINE)
    long_subject = "fix: " + "x" * 90
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def git(cwd: Path, *args: str) -> str:
            config = ["-c", "user.name=ci", "-c", "user.email=ci@example.com",
                      "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
                      "-c", "branch.autoSetupMerge=true", "-c", "init.defaultBranch=main"]
            result = subprocess.run(["git", *config, *args], cwd=cwd, check=True,
                                    capture_output=True, text=True)
            return result.stdout.strip()

        origin = root / "origin.git"
        work = root / "work"
        git(root, "init", "-q", "--bare", str(origin))
        git(root, "clone", "-q", str(origin), str(work))
        git(work, "commit", "-q", "--allow-empty", "-m", "chore: start")
        git(work, "push", "-q", "origin", "HEAD:main")

        for branch, subjects in (
            ("bad", ["fix: fine", "update stuff", long_subject]),
            ("good", ["fix: keep the range", 'Revert "fix: keep the range"']),
        ):
            git(work, "checkout", "-q", "-b", branch, "origin/main")
            for subject in subjects:
                git(work, "commit", "-q", "--allow-empty", "-m", subject)
        # main moves on, and good merges it back.
        git(work, "checkout", "-q", "-B", "main", "origin/main")
        git(work, "commit", "-q", "--allow-empty", "-m", "docs: later")
        git(work, "push", "-q", "origin", "main")
        git(work, "checkout", "-q", "good")
        git(work, "merge", "-q", "--no-ff", "main", "-m", "Merge branch 'main' into good")
        git(work, "push", "-q", "origin", "bad", "good")

        def run_as_ci(branch: str) -> tuple[subprocess.CompletedProcess, int]:
            ci = root / f"ci-{branch}"
            git(root, "clone", "-q", "--no-checkout", str(origin), str(ci))
            # What actions/checkout runs: the branch is recreated on, and tracks, its remote ref.
            git(ci, "checkout", "-q", "--force", "-B", branch, f"refs/remotes/origin/{branch}")
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith("GITHUB_") and k not in ("CHECK_RANGE", "BASE_BRANCH")}
            env.update(GITHUB_EVENT_NAME="push", GITHUB_SHA=git(ci, "rev-parse", "HEAD"))
            if base:
                env["BASE_BRANCH"] = base.group(1)
            result = subprocess.run([sys.executable, str(COMMIT_CHECKER)], cwd=ci, env=env,
                                    capture_output=True, text=True)
            checked = 0
            for line in result.stdout.splitlines():
                if line.startswith("Commit check range: "):
                    checked = int(git(ci, "rev-list", "--count", line.split(": ", 1)[1]))
            return result, checked

        result, checked = run_as_ci("bad")
        if result.returncode != 1 or "update stuff" not in result.stderr or long_subject not in result.stderr:
            print(f"  ❌ a branch with bad subjects exits {result.returncode} after checking "
                  f"{checked} commit(s), expected 1 and both subjects listed")
            ok = False

        result, checked = run_as_ci("good")
        if result.returncode != 0 or checked == 0:
            print(f"  ❌ conventional, revert and merge commits exit {result.returncode} after "
                  f"checking {checked} commit(s), expected 0 and a non-empty range")
            print("     " + result.stderr.strip().replace("\n", "\n     "))
            ok = False
    if ok:
        print("  ✅ CI commit checker range OK")
    print()
    return ok


def check_cycle_max_stack() -> bool:
    """
    #159: a function in a call cycle has no bounded depth, so its max stack is unknown, and so
    is that of every function that calls into a cycle, directly or not. The lower bound of a
    cycle member is its frame plus the largest of: the bound of a callee outside the cycle, the
    frame of a member it calls. It depends on the call graph only, not on the order of the
    definitions. Functions that reach no cycle keep a known max stack.
    """
    print("=== Testing the max stack of call cycles ===")
    sample = RUN_CONFIG.test_dir / "recursion/c/cycle-max-stack.c"
    result = run_analyzer(["--format=json", str(sample)])
    try:
        payload = json.loads(result.stdout or "")
    except json.JSONDecodeError as exc:
        print(f"  ❌ invalid JSON: {exc}")
        return False
    functions = {fn.get("name"): fn for fn in payload.get("functions", [])}

    # Every frame here is 16 bytes, but caller's, which is 0.
    # - cycle_c, cycle_d, other_e, other_f: 16 + the frame of the member each calls (16).
    # - self_loop: 16 + its own frame (16), the member it calls.
    # - enter_cycle: 16 + the bound of cycle_c (32); enter_twice: 16 + that of enter_cycle (48).
    unknown = {
        "cycle_c": 32,
        "cycle_d": 32,
        "other_e": 32,
        "other_f": 32,
        "self_loop": 32,
        "enter_cycle": 48,
        "enter_twice": 64,
    }
    known = {"leaf": 16, "caller": 16}
    ok = True
    for name, lower in unknown.items():
        fn = functions.get(name, {})
        got = (fn.get("maxStackUnknown"), fn.get("maxStack"), fn.get("maxStackLowerBound"))
        if got == (True, None, lower):
            print(f"  ✅ {name}: unknown, >= {lower} bytes")
        else:
            print(f"  ❌ {name}: expected unknown, >= {lower} bytes, got {got}")
            ok = False
    for name, total in known.items():
        fn = functions.get(name, {})
        got = (fn.get("maxStackUnknown"), fn.get("maxStack"))
        if got == (False, total):
            print(f"  ✅ {name}: {total} bytes")
        else:
            print(f"  ❌ {name}: expected {total} bytes, got {got}")
            ok = False
    print()
    return ok


def check_unresolved_call_max_stack() -> bool:
    """
    A function containing an unresolved call (indirect, or to an external
    declaration) has a max stack that is only a lower bound. It must be
    published as unknown by default, and --assume-external-frame=<bytes> must
    turn it back into a definite figure by charging <bytes> per such call.
    """
    print("=== Testing unresolved-call max stack policy ===")
    sample = RUN_CONFIG.test_dir / "local-storage/c/unresolved-call-max-stack.c"
    ok = True

    def functions_of(args: list[str], label: str):
        result = run_analyzer(["--format=json"] + args + [str(sample)])
        if result.returncode != 0:
            print(f"  ❌ {label}: analyzer failed (code {result.returncode})")
            print((result.stdout or "") + (result.stderr or ""))
            return None
        try:
            payload = json.loads(result.stdout or "")
        except json.JSONDecodeError as exc:
            print(f"  ❌ {label}: invalid JSON: {exc}")
            return None
        functions = {}
        for file_entry in payload.get("files", [payload]):
            for fn in file_entry.get("functions", []):
                functions[fn.get("name")] = fn
        return functions

    def expect_unknown(fns, name: str, label: str) -> bool:
        fn = fns.get(name)
        if fn is None:
            print(f"  ❌ {label}: function {name} missing from JSON")
            return False
        if fn.get("maxStackUnknown") is not True or fn.get("maxStack") is not None:
            print(f"  ❌ {label}: {name} should be unknown, got {fn}")
            return False
        if not isinstance(fn.get("maxStackLowerBound"), int) or fn["maxStackLowerBound"] <= 0:
            print(f"  ❌ {label}: {name} should keep a positive lower bound, got {fn}")
            return False
        return True

    def expect_known(fns, name: str, expected: int, label: str) -> bool:
        fn = fns.get(name)
        if fn is None:
            print(f"  ❌ {label}: function {name} missing from JSON")
            return False
        if fn.get("maxStackUnknown") is not False or fn.get("maxStack") != expected:
            print(f"  ❌ {label}: {name} expected maxStack={expected}, got {fn}")
            return False
        return True

    # Default: unknown with a lower bound, propagated to the caller.
    fns = functions_of([], "default")
    if fns is None:
        return False
    for name in ("calls_external", "calls_indirect", "caller"):
        ok = expect_unknown(fns, name, "default") and ok
    local_external = fns["calls_external"]["localStack"]
    local_indirect = fns["calls_indirect"]["localStack"]
    local_caller = fns["caller"]["localStack"]

    # Human output must render the lower bound the same way dynamic allocas do.
    human = run_analyzer([str(sample)])
    human_out = (human.stdout or "") + (human.stderr or "")
    needle = f"max stack (including callees): unknown (>= {local_external} bytes)"
    if needle not in human_out:
        print(f"  ❌ default: human output missing '{needle}'")
        print(human_out)
        ok = False

    # --assume-external-frame=<bytes> charges <bytes> per unresolved call.
    frame = 512
    fns = functions_of([f"--assume-external-frame={frame}"], "assume=512")
    if fns is None:
        return False
    ok = expect_known(fns, "calls_external", local_external + frame, "assume=512") and ok
    ok = expect_known(fns, "calls_indirect", local_indirect + frame, "assume=512") and ok
    deepest = max(local_external, local_indirect) + frame
    ok = expect_known(fns, "caller", local_caller + deepest, "assume=512") and ok

    # =0 restores the pre-#94 figures (unresolved calls cost nothing).
    fns = functions_of(["--assume-external-frame=0"], "assume=0")
    if fns is None:
        return False
    ok = expect_known(fns, "calls_external", local_external, "assume=0") and ok
    ok = expect_known(fns, "caller", local_caller + max(local_external, local_indirect), "assume=0") and ok

    # Config file key.
    with tempfile.NamedTemporaryFile("w", suffix=".cfg", delete=False) as cfg:
        cfg.write(f"assume-external-frame={frame}\n")
        cfg_path = cfg.name
    try:
        fns = functions_of(["--config", cfg_path], "config-key")
    finally:
        os.unlink(cfg_path)
    if fns is None:
        return False
    ok = expect_known(fns, "calls_external", local_external + frame, "config-key") and ok

    # Invalid values are rejected.
    for bad in ("--assume-external-frame=abc", "--assume-external-frame=-1"):
        result = run_analyzer([bad, str(sample)])
        if result.returncode == 0:
            print(f"  ❌ {bad} should be rejected")
            ok = False
    result = run_analyzer([str(sample), "--assume-external-frame"])
    output = (result.stdout or "") + (result.stderr or "")
    if result.returncode == 0 or "Missing argument for --assume-external-frame" not in output:
        print("  ❌ --assume-external-frame without a value should report a missing argument")
        print(output)
        ok = False

    if ok:
        print("  ✅ unresolved-call max stack policy OK\n")
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

    # The LLVM-free ownership engine has its own binary (built by the same option).
    engine_bin = RUN_CONFIG.analyzer.parent / "ownership_engine_unit_tests"
    if engine_bin.exists():
        engine = subprocess.run([str(engine_bin)], capture_output=True, text=True)
        engine_output = (engine.stdout or "") + (engine.stderr or "")
        if engine.returncode != 0:
            print(f"  ❌ ownership engine unit tests failed (code {engine.returncode})")
            print(engine_output)
            print()
            return False
        print("  ✅ ownership engine unit tests OK")
        if engine_output.strip():
            print(engine_output.rstrip())
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
        strict_details,
        unknown_scopes,
    ) = extract_expectations(c_path)
    if unknown_scopes:
        known = ", ".join(f"[{p}]" for p in _EXPECTATION_PASSES)
        report_lines.append(
            "  ❌ unknown expectation pass prefix: "
            + ", ".join(f"[{s}]" for s in sorted(set(unknown_scopes)))
            + f" (known: {known})"
        )
        return False, 1, 0, "\n".join(report_lines) + "\n\n"
    strict_enabled = (
        strict_diag_count if strict_diag_count is not None else _default_strict_diagnostic_count(c_path)
    )
    if not expectations and not negative_expectations and not strict_enabled:
        report_lines.append("  (no expectations found, skipping)")
        return True, 0, 0, "\n".join(report_lines) + "\n\n"

    def evaluate_pass(pass_name: str, analyzer_output: str):
        applicable = [text for scope, text in expectations if scope in (None, pass_name)]
        applicable_negative = [
            text for scope, text in negative_expectations if scope in (None, pass_name)
        ]
        pass_lines = [f"  [pass: {pass_name}]"]
        norm_output = normalize(analyzer_output)
        output_index = _build_output_diagnostic_index_by_location(analyzer_output)

        pass_ok = True
        pass_total = len(applicable) + len(applicable_negative)
        pass_passed = 0

        for idx, exp in enumerate(applicable, start=1):
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
            # The headline-only fallback ignores the "↳" detail lines entirely, so a
            # diagnostic's details can drift without any test noticing. Fixtures that pin a
            # computed value in a detail line opt out of it with:
            #   // strict-expectation-details: true
            if (
                not matched
                and not strict_details
                and _expectation_matches_by_location_and_headlines(exp, output_index)
            ):
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

        for idx, neg in enumerate(applicable_negative, start=1):
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
                1 for exp in applicable if _expectation_is_warning_or_error(exp)
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
    # With runner-level --smt args this run is neither the default pass nor the smt-z3 one:
    # only unscoped expectations describe it.
    baseline_pass = "custom-smt" if _runner_has_explicit_smt_args() else "default"
    base_ok, base_total, base_passed, base_lines = evaluate_pass(baseline_pass, baseline_output)
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
                f"--smt-timeout-ms={SMT_FIXTURE_TIMEOUT_MS}",
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
    RUN_CONFIG.analyzer_timeout = cli.analyzer_timeout if cli.analyzer_timeout > 0 else None
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
        check_smt_unavailable_backend_warning,
        check_analyzer_module_unit_tests,
        check_multi_file_json,
        check_multi_file_total_summary,
        check_multi_file_failure,
        check_cli_parsing_and_filters,
        check_unresolved_call_max_stack,
        check_cycle_max_stack,
        check_compile_ir_format_switch,
        check_pipeline_subscriber_rollout_parity,
        check_pipeline_timing_traversal_instrumentation,
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
        check_resource_model_across_compile_directories,
        check_ownership_cross_tu,
        check_ownership_wrapper_metadata,
        check_uninitialized_cross_tu,
        check_uninitialized_cross_tu_no_effect,
        check_const_param_cross_tu,
        check_duplicate_if_cross_tu,
        check_size_minus_one_cross_tu,
        check_cross_tu_call_graph,
        check_null_deref_nested_inter_tu,
        check_integer_overflow_advanced_inter_tu,
        check_noreturn_cross_tu,
        check_use_after_free_advanced_inter_tu,
        check_escape_model_rejects_unsupported_brackets,
        check_human_vs_json_parity,
        check_diagnostic_rule_coverage_regression,
        check_diagnostic_cwe_coverage,
        check_diagnostic_paths_follow_the_input,
        check_const_param_abi_split_struct,
        check_calls_through_aliases,
        check_resource_cache_rebuilds_alias_summaries,
        check_uninitialized_receiver_abi_split_struct,
        check_escape_through_returned_value,
        check_sarif_rule_cwe_tags,
        check_sarif_security_severity,
        check_ci_script_sarif_merge,
        check_ci_script_sarif_rule_join,
        check_ci_script_sarif_chunks,
        check_ci_commit_checker_range,
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
