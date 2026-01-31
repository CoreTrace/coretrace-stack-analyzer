#!/usr/bin/env python3
import sys
import subprocess
import json
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
    lines = c_path.read_text().splitlines()
    i = 0
    n = len(lines)

    while i < n:
        raw = lines[i]
        stripped = raw.lstrip()

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

    return expectations, negative_expectations


def run_analyzer_on_file(c_path: Path) -> str:
    """
    Lance ton analyseur sur un fichier C et récupère stdout+stderr.
    """
    result = subprocess.run(
        [str(ANALYZER), str(c_path)],
        capture_output=True,
        text=True,
    )
    output = (result.stdout or "") + (result.stderr or "")
    return output


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
    expectations, negative_expectations = extract_expectations(c_path)
    if not expectations and not negative_expectations:
        print("  (no expectations found, skipping)\n")
        return True, 0, 0

    analyzer_output = run_analyzer_on_file(c_path)
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
    global_ok = check_help_flags()
    if not check_multi_file_json():
        global_ok = False
    if not check_multi_file_failure():
        global_ok = False
    if not check_cli_parsing_and_filters():
        global_ok = False
    total_tests = 0
    passed_tests = 0

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
