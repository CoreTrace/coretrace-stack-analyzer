#!/usr/bin/env python3
from __future__ import annotations

import os
import re
import subprocess
import sys
from typing import Iterable, List, Optional, Tuple

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

CONVENTIONAL_RE = re.compile(
    r"^(?P<type>" + "|".join(ALLOWED_TYPES) + r")"
    r"(?P<scope>\([^)]+\))?"
    r"(?P<breaking>!)?: "
    r"(?P<subject>.+)$"
)

BREAKING_RE = re.compile(r"^BREAKING[ -]CHANGE:", re.MULTILINE)


class Commit:
    def __init__(self, sha: str, subject: str, body: str) -> None:
        self.sha = sha
        self.subject = subject
        self.body = body


def run_git(args: List[str]) -> str:
    try:
        out = subprocess.check_output(["git", *args], text=True).strip()
    except subprocess.CalledProcessError as exc:
        print(exc.output, file=sys.stderr)
        raise
    return out


def is_zero_sha(value: str) -> bool:
    return re.fullmatch(r"0+", value or "") is not None


def get_latest_tag() -> Optional[str]:
    tags = run_git(["tag", "--list", "v[0-9]*.[0-9]*.[0-9]*", "--sort=-v:refname"])
    if not tags:
        return None
    return tags.splitlines()[0].strip()


def parse_version(tag: Optional[str]) -> Tuple[int, int, int]:
    if not tag:
        return (0, 0, 0)
    match = re.match(r"^v(\d+)\.(\d+)\.(\d+)$", tag)
    if not match:
        return (0, 0, 0)
    return tuple(int(p) for p in match.groups())


def iter_commits(range_spec: Optional[str]) -> Iterable[Commit]:
    args = ["log", "--format=%H%x1f%s%x1f%b%x1e"]
    if range_spec:
        args.insert(1, range_spec)
    raw = run_git(args)
    if not raw:
        return []
    commits: List[Commit] = []
    for record in raw.split("\x1e"):
        record = record.strip()
        if not record:
            continue
        parts = record.split("\x1f")
        if len(parts) < 2:
            continue
        sha = parts[0]
        subject = parts[1]
        body = parts[2] if len(parts) > 2 else ""
        commits.append(Commit(sha=sha, subject=subject, body=body))
    return commits


def is_merge_commit(commit: Commit) -> bool:
    subject = commit.subject
    return subject.startswith("Merge ") or subject.startswith("Merge pull request")


def classify_bump(commits: Iterable[Commit]) -> str:
    bump = "none"
    for commit in commits:
        if is_merge_commit(commit):
            continue
        match = CONVENTIONAL_RE.match(commit.subject)
        if not match:
            continue
        if match.group("breaking") or BREAKING_RE.search(commit.body or ""):
            return "major"
        ctype = match.group("type")
        if ctype == "feat" and bump != "minor":
            bump = "minor"
        elif ctype == "fix" and bump == "none":
            bump = "patch"
    return bump


def bump_version(base: Tuple[int, int, int], bump: str) -> Tuple[int, int, int]:
    major, minor, patch = base
    if bump == "major":
        return (major + 1, 0, 0)
    if bump == "minor":
        return (major, minor + 1, 0)
    if bump == "patch":
        return (major, minor, patch + 1)
    return base


def write_output(key: str, value: str) -> None:
    output_path = os.environ.get("GITHUB_OUTPUT")
    if not output_path:
        return
    with open(output_path, "a", encoding="utf-8") as fh:
        fh.write(f"{key}={value}\n")


def main() -> int:
    base_tag = get_latest_tag()
    base_version = parse_version(base_tag)
    version_range = os.environ.get("VERSION_RANGE")
    if not version_range:
        version_range = f"{base_tag}..HEAD" if base_tag else "HEAD"

    version_commits = list(iter_commits(version_range))
    bump = classify_bump(version_commits)
    next_version = bump_version(base_version, bump)

    base_tag_out = base_tag or ""
    next_version_str = f"v{next_version[0]}.{next_version[1]}.{next_version[2]}"
    next_version_raw = f"{next_version[0]}.{next_version[1]}.{next_version[2]}"

    write_output("base_tag", base_tag_out)
    write_output("bump", bump)
    write_output("next_version", next_version_str)
    write_output("next_version_raw", next_version_raw)

    print(f"Base tag: {base_tag_out or 'none'}")
    print(f"Version range: {version_range}")
    print(f"Bump: {bump}")
    print(f"Next version: {next_version_str}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
