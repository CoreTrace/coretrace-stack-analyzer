#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

cd "${REPO_ROOT}"

if [[ ! -f "LICENSE" ]]; then
    echo "ERROR: Missing LICENSE file at repository root." >&2
    exit 1
fi

if ! grep -Eq 'Apache License' LICENSE || ! grep -Eq 'Version 2\.0,[[:space:]]*January 2004' LICENSE; then
    echo "ERROR: LICENSE file does not match Apache License 2.0 header markers." >&2
    exit 1
fi

missing_spdx_headers=()

while IFS= read -r -d '' file; do
    case "${file}" in
        build/*|build-*/*|extern-project/build/*|extern-project/build-*/*|third_party/*|vendor/*|external/*)
            continue
            ;;
        CMakeLists.txt|main.cpp|*.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.ipp|*.tpp|*.cmake|*.sh|*.py|*.yml|*.yaml)
            ;;
        *)
            continue
            ;;
    esac

    if ! head -n 12 "${file}" | grep -Eq 'SPDX-License-Identifier:[[:space:]]*Apache-2\.0'; then
        missing_spdx_headers+=("${file}")
    fi
done < <(git ls-files -z)

if ((${#missing_spdx_headers[@]} > 0)); then
    echo "ERROR: Missing Apache-2.0 SPDX headers in these files:" >&2
    printf '  - %s\n' "${missing_spdx_headers[@]}" >&2
    exit 1
fi

echo "License compliance checks passed."
