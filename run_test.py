#!/usr/bin/env python3
import sys
import subprocess
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
    lines = c_path.read_text().splitlines()
    i = 0
    n = len(lines)

    while i < n:
        raw = lines[i]
        stripped = raw.lstrip()

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

    return expectations


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


def check_file(c_path: Path):
    """
    Vérifie qu'avec ce fichier, toutes les attentes sont présentes
    dans la sortie de l'analyseur.
    """
    print(f"=== Testing {c_path} ===")
    expectations = extract_expectations(c_path)
    if not expectations:
        print("  (no expectations found, skipping)\n")
        return True, 0, 0

    analyzer_output = run_analyzer_on_file(c_path)
    norm_output = normalize(analyzer_output)

    all_ok = True
    total = len(expectations)
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

    print()
    return all_ok, total, passed


def main() -> int:
    global_ok = check_help_flags()
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
