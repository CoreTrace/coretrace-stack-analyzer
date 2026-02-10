#!/usr/bin/env python3
from __future__ import annotations

import re
import sys
from typing import List

ALLOWED_TYPES = (
    "feat",
    "fix",
    "chore",
    "docs",
    "refactor",
    "perf",
    "ci",
    "build",
    "style",
    "revert",
    "test",
)

MAX_SUBJECT_LEN = 84

CONVENTIONAL_RE = re.compile(
    r"^(?P<type>" + "|".join(ALLOWED_TYPES) + r")"
    r"(?P<scope>\([^)]+\))?"
    r"(?P<breaking>!)?: "
    r"(?P<subject>.+)$"
)


def read_subject(path: str) -> str:
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            stripped = line.strip()
            if not stripped:
                continue
            if stripped.startswith("#"):
                continue
            return stripped
    return ""


def is_merge_message(subject: str) -> bool:
    return subject.startswith("Merge ") or subject.startswith("Merge pull request")


def is_git_revert(subject: str) -> bool:
    return subject.startswith("Revert ")


def main(argv: List[str]) -> int:
    if len(argv) < 2:
        print("commit message file path missing", file=sys.stderr)
        return 1

    subject = read_subject(argv[1])
    if not subject:
        print("empty commit message", file=sys.stderr)
        return 1

    if is_merge_message(subject) or is_git_revert(subject):
        return 0

    if len(subject) > MAX_SUBJECT_LEN:
        print(
            f"Commit subject too long ({len(subject)} > {MAX_SUBJECT_LEN}).",
            file=sys.stderr,
        )
        return 1

    if not CONVENTIONAL_RE.match(subject):
        print("Commit message must follow Conventional Commits.", file=sys.stderr)
        print(f"Got: {subject}", file=sys.stderr)
        print(
            "Expected: <type>(<scope>)?: <subject> with type in: "
            + ", ".join(ALLOWED_TYPES),
            file=sys.stderr,
        )
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
