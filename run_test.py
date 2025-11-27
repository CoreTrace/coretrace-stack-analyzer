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
        lines.append(" ".join(parts))
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


def check_file(c_path: Path) -> bool:
    """
    Vérifie qu'avec ce fichier, toutes les attentes sont présentes
    dans la sortie de l'analyseur.
    """
    print(f"=== Testing {c_path} ===")
    expectations = extract_expectations(c_path)
    if not expectations:
        print("  (no expectations found, skipping)\n")
        return True

    analyzer_output = run_analyzer_on_file(c_path)
    norm_output = normalize(analyzer_output)

    all_ok = True
    for idx, exp in enumerate(expectations, start=1):
        norm_exp = normalize(exp)
        if norm_exp in norm_output:
            print(f"  ✅ expectation #{idx} FOUND")
        else:
            print(f"  ❌ expectation #{idx} MISSING")
            print("----- Expected block -----")
            print(exp)
            print("----- Analyzer output (normalized) -----")
            # tu peux commenter cette ligne si l'output est trop gros
            # print(norm_output)
            print("---------------------------")
            all_ok = False

    print()
    return all_ok


def main() -> int:
    c_files = sorted(TEST_DIR.glob("**/*.c"))
    if not c_files:
        print(f"No .c files found under {TEST_DIR}")
        return

    global_ok = True
    for f in c_files:
        ok = check_file(f)
        if not ok:
            global_ok = False

    if global_ok:
        print("✅ All tests passed.")
        return 0
    else:
        print("❌ Some tests failed.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
