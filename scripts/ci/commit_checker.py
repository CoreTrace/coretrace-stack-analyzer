#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass
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

MAX_SUBJECT_LEN = 84

CONVENTIONAL_RE = re.compile(
    r"^(?P<type>" + "|".join(ALLOWED_TYPES) + r")"
    r"(?P<scope>\([^)]+\))?"
    r"(?P<breaking>!)?: "
    r"(?P<subject>.+)$"
)


@dataclass
class Commit:
    sha: str
    subject: str
    body: str


@dataclass
class InvalidCommit:
    commit: Commit
    reason: str


def run_git(args: List[str]) -> str:
    try:
        out = subprocess.check_output(["git", *args], text=True).strip()
    except subprocess.CalledProcessError as exc:
        print(exc.output, file=sys.stderr)
        raise
    return out


def get_event_range() -> Optional[str]:
    override = os.environ.get("CHECK_RANGE")
    if override:
        return override

    event = os.environ.get("GITHUB_EVENT_NAME", "")
    if event == "pull_request":
        event_path = os.environ.get("GITHUB_EVENT_PATH")
        if not event_path or not os.path.exists(event_path):
            return None
        with open(event_path, "r", encoding="utf-8") as fh:
            payload = json.load(fh)
        base_sha = payload.get("pull_request", {}).get("base", {}).get("sha")
        head_sha = payload.get("pull_request", {}).get("head", {}).get("sha")
        if base_sha and head_sha:
            return f"{base_sha}..{head_sha}"
        return None

    if event == "push":
        before = os.environ.get("GITHUB_BEFORE")
        head = os.environ.get("GITHUB_SHA")
        if before and head and not is_zero_sha(before):
            return f"{before}..{head}"
        if head:
            return None

    return None


def is_zero_sha(value: str) -> bool:
    return re.fullmatch(r"0+", value or "") is not None


def range_for_ref(ref: str) -> str:
    try:
        run_git(["rev-parse", f"{ref}^"])
    except subprocess.CalledProcessError:
        return ref
    return f"{ref}^..{ref}"


def get_upstream_ref() -> Optional[str]:
    try:
        upstream = run_git(["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"])
    except subprocess.CalledProcessError:
        return None
    return upstream or None


def find_base_ref() -> Optional[str]:
    explicit = os.environ.get("BASE_BRANCH")
    candidates: List[str] = []
    if explicit:
        candidates.extend([explicit, f"origin/{explicit}"])

    upstream = get_upstream_ref()
    if upstream:
        candidates.append(upstream)

    candidates.extend(["origin/main", "origin/master", "main", "master"])

    seen = set()
    for ref in candidates:
        if ref in seen:
            continue
        seen.add(ref)
        try:
            run_git(["rev-parse", "--verify", ref])
            return ref
        except subprocess.CalledProcessError:
            continue
    return None


def compute_branch_range() -> str:
    base_ref = find_base_ref()
    if not base_ref:
        return range_for_ref("HEAD")
    try:
        base_sha = run_git(["merge-base", "HEAD", base_ref])
    except subprocess.CalledProcessError:
        return range_for_ref("HEAD")
    if not base_sha:
        return range_for_ref("HEAD")
    return f"{base_sha}..HEAD"


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


def validate_commits(commits: Iterable[Commit]) -> List[InvalidCommit]:
    invalid: List[InvalidCommit] = []
    for commit in commits:
        if is_merge_commit(commit):
            continue
        if len(commit.subject) > MAX_SUBJECT_LEN:
            invalid.append(
                InvalidCommit(
                    commit=commit,
                    reason=f"subject too long ({len(commit.subject)} > {MAX_SUBJECT_LEN})",
                )
            )
            continue
        if not CONVENTIONAL_RE.match(commit.subject):
            invalid.append(InvalidCommit(commit=commit, reason="invalid conventional format"))
    return invalid


def main() -> int:
    check_range = get_event_range()
    if not check_range:
        check_range = compute_branch_range()
    print(f"Commit check range: {check_range}")
    commits = list(iter_commits(check_range))
    invalid = validate_commits(commits)
    if invalid:
        print("Non-conventional commits detected:", file=sys.stderr)
        for entry in invalid:
            print(
                f"- {entry.commit.sha[:7]} {entry.commit.subject} ({entry.reason})",
                file=sys.stderr,
            )
        return 1

    print("All commits follow Conventional Commits.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
