#!/usr/bin/env bash
set -euo pipefail

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required to install pre-commit" >&2
  exit 1
fi

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
VENV_DIR="${ROOT_DIR}/.venv-pre-commit"

python3 -m venv "${VENV_DIR}"
source "${VENV_DIR}/bin/activate"
python -m pip install --upgrade pip >/dev/null 2>&1
python -m pip install pre-commit >/dev/null 2>&1
python -m pre_commit install --hook-type commit-msg

echo "Commit-msg hook installed using ${VENV_DIR}."
