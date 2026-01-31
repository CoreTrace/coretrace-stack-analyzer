#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

# Collect source files while skipping generated / external content.
files=()
while IFS= read -r -d '' file; do
    files+=("$file")
done < <(find "${REPO_ROOT}" \
    \( -path "${REPO_ROOT}/build" -o -path "${REPO_ROOT}/extern-project" -o -path "${REPO_ROOT}/external" -o -path "${REPO_ROOT}/third_party" -o -path "${REPO_ROOT}/vendor" -o -path "${REPO_ROOT}/.git" \) -prune -o \
    -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) -print0)

if [ "${#files[@]}" -eq 0 ]; then
    echo "No source files to format."
    exit 0
fi

if [ -n "${CLANG_FORMAT:-}" ]; then
    CF_BIN="${CLANG_FORMAT}"
elif command -v clang-format-20 >/dev/null 2>&1; then
    CF_BIN="clang-format-20"
elif command -v clang-format >/dev/null 2>&1; then
    CF_BIN="clang-format"
else
    CF_BIN="clang-format"
fi

if ! command -v "${CF_BIN}" >/dev/null 2>&1; then
    echo "clang-format binary not found: ${CF_BIN}"
    echo "Set CLANG_FORMAT or install clang-format."
    exit 1
fi

if [ "${CF_BIN}" = "clang-format" ]; then
    version_line="$(${CF_BIN} --version 2>/dev/null || true)"
    if [[ "${version_line}" =~ ([0-9]+)\. ]]; then
        major="${BASH_REMATCH[1]}"
        if [ "${major}" -lt 20 ]; then
            echo "clang-format version too old (${version_line}). Need >= 20."
            echo "Set CLANG_FORMAT to clang-format-20 or install a newer clang-format."
            exit 1
        fi
    fi
fi

echo "Formatting ${#files[@]} files with ${CF_BIN} (style from ${REPO_ROOT}/.clang-format)..."
"${CF_BIN}" -i "${files[@]}"
