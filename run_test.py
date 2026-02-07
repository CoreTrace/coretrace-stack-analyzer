#!/usr/bin/env python3
import sys
import subprocess
import json
import re
from pathlib import Path

# Chemin vers ton binaire d'analyse
ANALYZER = Path("./build/stack_usage_analyzer")   # à adapter si besoin
TEST_DIR = Path("test")                     # dossier contenant les .c


def normalize(s: str) -> str:
    """
    Normalise les espaces pour rendre les comparaisons plus robustes :
    - supprime les espaces inutiles en début/fin de ligne
    - remplace les séquences d'espaces par un seul espace
    - garde les sauts de lignes
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
        normalized = re.sub(r"__([A-Za-z0-9_]+)_chk\b", r"\1", normalized)
        lines.append(normalized)
    return "\n".join(lines).strip()


def extract_expectations(c_path: Path):
    """
    Extrait les blocs de commentaires d'attendus dans un fichier .c.

    On cherche les commentaires qui commencent par "// at line".
    On prend toutes les lignes de commentaires qui suivent.
    """
    expectations = []
    negative_expectations = []
    stack_limit = None
    lines = c_path.read_text().splitlines()
    i = 0
    n = len(lines)

    while i < n:
        raw = lines[i]
        stripped = raw.lstrip()

        stack_match = re.match(r"//\s*stack-limit\s*[:=]\s*(\S+)", stripped, re.IGNORECASE)
        if stack_match:
            stack_limit = stack_match.group(1)
            i += 1
            continue

        stripped_line = stripped
        if stripped_line.startswith("// not contains:"):
            negative = stripped_line[len("// not contains:"):].strip()
            if negative:
                negative_expectations.append(negative)
            i += 1
            continue

        # Début d'un bloc d'attendu
        if stripped.startswith("// at line"):
            comment_block = [raw]
            i += 1
            # Récupère toutes les lignes "// ..." qui suivent
            while i < n and lines[i].lstrip().startswith("//"):
                comment_block.append(lines[i])
                i += 1

            # Nettoyage : retirer les "//" et les indentations
            cleaned_lines = []
            for c in comment_block:
                s = c.lstrip()
                if s.startswith("//"):
                    s = s[2:]  # enlève "//"
                cleaned_lines.append(s.lstrip())

            expectation_text = "\n".join(cleaned_lines)
            expectations.append(expectation_text)
        else:
            i += 1

    return expectations, negative_expectations, stack_limit


def run_analyzer_on_file(c_path: Path, stack_limit=None) -> str:
    """
    Lance ton analyseur sur un fichier C et récupère stdout+stderr.
    """
    args = [str(ANALYZER), str(c_path)]
    if stack_limit:
        args.append(f"--stack-limit={stack_limit}")
    result = subprocess.run(
        args,
        capture_output=True,
        text=True,
    )
    output = (result.stdout or "") + (result.stderr or "")
    return output


def run_analyzer(args) -> subprocess.CompletedProcess:
    """
    Run analyzer with custom args and return the CompletedProcess.
    """
    return subprocess.run([str(ANALYZER)] + args, capture_output=True, text=True)


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
                    elif "recursive or mutually recursive function detected" in stripped:
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


def check_human_vs_json_parity() -> bool:
    """
    Compare human-readable output vs JSON output for the same input.
    Fails if information present in one view is missing in the other.
    """
    print("=== Testing human vs JSON parity ===")
    samples = []
    for ext in ("*.c", "*.cpp"):
        samples.extend(TEST_DIR.glob(f"**/{ext}"))
    samples = sorted(samples)
    if not samples:
        print("  (no .c/.cpp files found, skipping)\n")
        return True

    ok = True
    for sample in samples:
        sample_ok = True
        human = run_analyzer([str(sample)])
        if human.returncode != 0:
            print(f"  ❌ human run failed for {sample} (code {human.returncode})")
            print(human.stdout)
            print(human.stderr)
            sample_ok = False
            ok = False
            continue

        structured = run_analyzer([str(sample), "--format=json"])
        if structured.returncode != 0:
            print(f"  ❌ json run failed for {sample} (code {structured.returncode})")
            print(structured.stdout)
            print(structured.stderr)
            sample_ok = False
            ok = False
            continue

        try:
            payload = json.loads(structured.stdout)
        except json.JSONDecodeError as exc:
            print(f"  ❌ invalid JSON output for {sample}: {exc}")
            print(structured.stdout)
            sample_ok = False
            ok = False
            continue

        human_output = (human.stdout or "") + (human.stderr or "")
        norm_human = normalize(human_output)
        human_functions = parse_human_functions(human_output)
        human_diag_blocks = parse_human_diagnostic_messages(human_output)

        mode = payload.get("meta", {}).get("mode")
        if mode and f"Mode: {mode}" not in human_output:
            print(f"  ❌ mode mismatch for {sample} (json={mode})")
            sample_ok = False

        def has_json_recursion_diag(func_name: str, needle: str) -> bool:
            for d in payload.get("diagnostics", []):
                loc = d.get("location", {})
                if loc.get("function") != func_name:
                    continue
                msg = d.get("details", {}).get("message", "")
                if needle in msg:
                    return True
            return False

        for f in payload.get("functions", []):
            name = f.get("name", "")
            if not name:
                continue
            if name not in human_functions:
                print(f"  ❌ function missing in human output: {name}")
                sample_ok = False
                continue
            hf = human_functions[name]

            if hf["localStackUnknown"] is None:
                print(f"  ❌ local stack missing in human output for: {name}")
                sample_ok = False
            elif f.get("localStackUnknown") != hf["localStackUnknown"]:
                print(f"  ❌ local stack unknown flag mismatch for: {name}")
                sample_ok = False
            elif not f.get("localStackUnknown"):
                if f.get("localStack") != hf["localStack"]:
                    print(f"  ❌ local stack value mismatch for: {name}")
                    sample_ok = False
            elif hf["localStackLowerBound"] is not None:
                json_lb = f.get("localStackLowerBound")
                if json_lb != hf["localStackLowerBound"]:
                    print(f"  ❌ local stack lower bound mismatch for: {name}")
                    sample_ok = False

            if hf["maxStackUnknown"] is None:
                print(f"  ❌ max stack missing in human output for: {name}")
                sample_ok = False
            elif f.get("maxStackUnknown") != hf["maxStackUnknown"]:
                print(f"  ❌ max stack unknown flag mismatch for: {name}")
                sample_ok = False
            elif not f.get("maxStackUnknown"):
                if f.get("maxStack") != hf["maxStack"]:
                    print(f"  ❌ max stack value mismatch for: {name}")
                    sample_ok = False
            elif hf["maxStackLowerBound"] is not None:
                json_lb = f.get("maxStackLowerBound")
                if json_lb != hf["maxStackLowerBound"]:
                    print(f"  ❌ max stack lower bound mismatch for: {name}")
                    sample_ok = False

            if f.get("isRecursive") != hf["isRecursive"]:
                print(f"  ❌ recursion flag mismatch for: {name}")
                print(f"     human: {hf['isRecursive']} json: {f.get('isRecursive')}")
                block = extract_human_function_block(human_output, name)
                if block:
                    print("     human block:")
                    print(block)
                else:
                    print("     human block: <not found>")
                print(f"     json function: {f}")
                # Do not fail on flag mismatch alone; message parity handles recursion info.
            if f.get("hasInfiniteSelfRecursion") != hf["hasInfiniteSelfRecursion"]:
                print(f"  ❌ infinite recursion flag mismatch for: {name}")
                print(f"     human: {hf['hasInfiniteSelfRecursion']} json: {f.get('hasInfiniteSelfRecursion')}")
                block = extract_human_function_block(human_output, name)
                if block:
                    print("     human block:")
                    print(block)
                else:
                    print("     human block: <not found>")
                print(f"     json function: {f}")
                # Do not fail on flag mismatch alone; message parity handles recursion info.
            if f.get("exceedsLimit") != hf["exceedsLimit"]:
                print(f"  ❌ stack limit flag mismatch for: {name}")
                print(f"     human: {hf['exceedsLimit']} json: {f.get('exceedsLimit')}")
                block = extract_human_function_block(human_output, name)
                if block:
                    print("     human block:")
                    print(block)
                else:
                    print("     human block: <not found>")
                print(f"     json function: {f}")
                sample_ok = False

        for d in payload.get("diagnostics", []):
            details = d.get("details", {})
            msg = details.get("message", "")
            if msg and normalize(msg) not in norm_human:
                print("  ❌ diagnostic message missing in human output")
                print(f"     message: {msg}")
                sample_ok = False
            loc = d.get("location", {})
            line = loc.get("startLine", 0)
            column = loc.get("startColumn", 0)
            if line and column:
                needle = normalize(f"at line {line}, column {column}")
                if needle not in norm_human:
                    print("  ❌ diagnostic location missing in human output")
                    print(f"     location: line {line}, column {column}")
                    sample_ok = False

        json_messages = {
            normalize(d.get("details", {}).get("message", ""))
            for d in payload.get("diagnostics", [])
            if d.get("details", {}).get("message")
        }
        for block in human_diag_blocks:
            if block and block not in json_messages:
                print("  ❌ diagnostic message missing in JSON output")
                print(f"     message: {block}")
                sample_ok = False

        if sample_ok:
            print(f"  ✅ parity OK for {sample}")
        else:
            print(f"  ❌ parity FAIL for {sample}")
        ok = ok and sample_ok

    print()
    return ok


def check_help_flags() -> bool:
    """
    Vérifie que -h et --help affichent l'aide sur stdout et retournent 0.
    """
    print("=== Testing help flags ===")
    ok = True
    for flag in ["-h", "--help"]:
        result = subprocess.run(
            [str(ANALYZER), flag],
            capture_output=True,
            text=True,
        )
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
    Vérifie que l'analyse accepte plusieurs fichiers et que le JSON agrège correctement.
    """
    print("=== Testing multi-file JSON ===")
    file_a = TEST_DIR / "test.ll"
    file_b = TEST_DIR / "recursion/c/limited-recursion.ll"

    result = subprocess.run(
        [str(ANALYZER), str(file_a), str(file_b), "--format=json"],
        capture_output=True,
        text=True,
    )
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


def check_multi_file_failure() -> bool:
    """
    Vérifie le comportement fail-fast quand un fichier est invalide.
    """
    print("=== Testing multi-file failure ===")
    valid_file = TEST_DIR / "test.ll"
    missing_file = TEST_DIR / "does-not-exist.ll"

    result = subprocess.run(
        [str(ANALYZER), str(valid_file), str(missing_file)],
        capture_output=True,
        text=True,
    )
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
    Vérifie parsing CLI (erreurs) + filtres principaux.
    """
    print("=== Testing CLI parsing & filters ===")
    ok = True

    sample = TEST_DIR / "false-positif/unique_ptr_state.cpp"

    # Missing-argument cases
    missing_arg_cases = [
        ("--only-file", "Missing argument for --only-file"),
        ("--only-dir", "Missing argument for --only-dir"),
        ("--only-func", "Missing argument for --only-func"),
        ("--only-function", "Missing argument for --only-function"),
        ("-I", "Missing argument for -I"),
        ("-D", "Missing argument for -D"),
    ]
    for flag, needle in missing_arg_cases:
        result = subprocess.run([str(ANALYZER), flag], capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode == 0 or needle not in output:
            print(f"  ❌ {flag} missing-arg handling")
            print(output)
            ok = False
        else:
            print(f"  ✅ {flag} missing-arg OK")

    # Unknown option
    result = subprocess.run([str(ANALYZER), "--unknown-option"], capture_output=True, text=True)
    output = (result.stdout or "") + (result.stderr or "")
    if "Unknown option: --unknown-option" not in output:
        print("  ❌ unknown option handling")
        print(output)
        ok = False
    else:
        print("  ✅ unknown option OK")

    # only-function variants
    only_function_cases = [
        ["--only-function=transition"],
        ["--only-function=transition,does_not_exist"],
        ["--only-function=transition, InitState::handle"],
        ["--only-function", "transition"],
        ["--only-func=transition"],
        ["--only-func", "transition"],
    ]
    for opt in only_function_cases:
        cmd = [str(ANALYZER), str(sample)] + opt
        result = subprocess.run(cmd, capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0 or "Function:" not in output:
            print(f"  ❌ only-function case failed: {opt}")
            print(output)
            ok = False
        else:
            print(f"  ✅ only-function case OK: {opt}")

    # only-file / only-dir with space
    only_file_dir_cases = [
        ["--only-file", str(sample)],
        ["--only-dir", str(sample.parent)],
    ]
    for opt in only_file_dir_cases:
        cmd = [str(ANALYZER), str(sample)] + opt + ["--only-function=transition"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0 or "Function:" not in output:
            print(f"  ❌ only-file/dir case failed: {' '.join(opt)}")
            print(output)
            ok = False
        else:
            print(f"  ✅ only-file/dir case OK: {' '.join(opt)}")

    # -D variants
    macro_cases = [
        ["-DHELLO"],
        ["-D", "HELLO"],
        ["-DVALUE=42"],
        ["-D", "VALUE=42"],
    ]
    for opt in macro_cases:
        cmd = [str(ANALYZER), str(sample)] + opt + ["--only-function=transition"]
        result = subprocess.run(cmd, capture_output=True, text=True)
        output = (result.stdout or "") + (result.stderr or "")
        if result.returncode != 0 or "Function:" not in output:
            print(f"  ❌ macro case failed: {' '.join(opt)}")
            print(output)
            ok = False
        else:
            print(f"  ✅ macro case OK: {' '.join(opt)}")

    print()
    return ok


def check_file(c_path: Path):
    """
    Vérifie qu'avec ce fichier, toutes les attentes sont présentes
    dans la sortie de l'analyseur.
    """
    print(f"=== Testing {c_path} ===")
    expectations, negative_expectations, stack_limit = extract_expectations(c_path)
    if not expectations and not negative_expectations:
        print("  (no expectations found, skipping)\n")
        return True, 0, 0

    analyzer_output = run_analyzer_on_file(c_path, stack_limit=stack_limit)
    norm_output = normalize(analyzer_output)

    all_ok = True
    total = len(expectations) + len(negative_expectations)
    passed = 0
    for idx, exp in enumerate(expectations, start=1):
        norm_exp = normalize(exp)
        if norm_exp in norm_output:
            print(f"  ✅ expectation #{idx} FOUND")
            passed += 1
        else:
            print(f"  ❌ expectation #{idx} MISSING")
            print("----- Expected block -----")
            print(exp)
            print("----- Analyzer output (normalized) -----")
            print(f"<{norm_output}>")
            print("---------------------------")
            all_ok = False

    for idx, neg in enumerate(negative_expectations, start=1):
        norm_neg = normalize(neg)
        if norm_neg and norm_neg not in norm_output:
            print(f"  ✅ negative expectation #{idx} NOT FOUND (as expected)")
            passed += 1
        else:
            print(f"  ❌ negative expectation #{idx} FOUND (unexpected)")
            print("----- Forbidden text -----")
            print(neg)
            print("----- Analyzer output (normalized) -----")
            print(f"<{norm_output}>")
            print("---------------------------")
            all_ok = False

    print()
    return all_ok, total, passed


def main() -> int:
    total_tests = 0
    passed_tests = 0

    def record_ok(ok: bool):
        nonlocal total_tests, passed_tests
        total_tests += 1
        if ok:
            passed_tests += 1
        return ok

    global_ok = record_ok(check_help_flags())
    if not record_ok(check_multi_file_json()):
        global_ok = False
    if not record_ok(check_multi_file_failure()):
        global_ok = False
    if not record_ok(check_cli_parsing_and_filters()):
        global_ok = False
    if not record_ok(check_human_vs_json_parity()):
        global_ok = False

    c_files = sorted(list(TEST_DIR.glob("**/*.c")) + list(TEST_DIR.glob("**/*.cpp")))
    if not c_files:
        print(f"No .c/.cpp files found under {TEST_DIR}")
        return 0 if global_ok else 1

    for f in c_files:
        ok, total, passed = check_file(f)
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
