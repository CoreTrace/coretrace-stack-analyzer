# Security Policy

## Supported Versions

`coretrace-stack-analyzer` is pre-1.0 and does not maintain release branches.
Security fixes land on `main` and ship in the next minor tag; there is no
backporting to earlier `0.x` lines.

| Version                    | Supported          |
| -------------------------- | ------------------ |
| Latest `0.x` minor (v0.19) | :white_check_mark: |
| Older `0.x` minors         | :x:                |

This applies to every distribution channel:

- **GitHub Action** — consumers pinning `CoreTrace/coretrace-stack-analyzer@v0`
  receive fixes when the floating `v0` tag is moved. Pinning an exact tag or
  commit SHA is safer, but then upgrading after an advisory is on you.
- **Container images** — `ghcr.io/coretrace/coretrace-stack-analyzer` and
  `-ci`. Only the most recent tag is rebuilt.
- **Release binaries** — attached to `v*` tags. Not rebuilt retroactively.

Once 1.0 ships this table is expected to change; until then, treat "supported"
as "the latest tag, and nothing else".

## Reporting a Vulnerability

Please report privately, through either channel:

- **GitHub** — the **Report a vulnerability** button on the
  [Security tab](https://github.com/CoreTrace/coretrace-stack-analyzer/security/advisories/new).
  Preferred: it keeps the report, the fix and the advisory in one place.
- **E-mail** — `hugo.payet@epitech.eu`, if you would rather not use GitHub.

Please do not open a public issue, PR, or discussion for a suspected
vulnerability.

Include, where you can:

- affected version or commit SHA, and the distribution channel (Action, image,
  binary, source build);
- the exact invocation — CLI flags, `.ctrace-analyzer.cfg`, and whether a
  `compile_commands.json` was involved;
- a minimal reproducer (source file, IR, or compile database);
- observed versus expected behaviour, and the impact you believe it has.

### What to expect

This is a small project and does not publish response-time targets it cannot
reliably meet. Reports are triaged as soon as we are able, prioritised by
severity and exploitability, and you will get a human reply rather than
silence.

Concretely:

- we confirm receipt and tell you whether the report is understood, or what
  further detail we need;
- we tell you whether it is accepted as a vulnerability, tracked as an ordinary
  bug, or considered out of scope — with the reasoning, not just the verdict;
- if accepted, you get the fix, the release it ships in, and credit in a GitHub
  Security Advisory unless you ask to stay anonymous;
- if you ask where a report stands, you get a straight answer about where it
  stands.

We will not ask you for an open-ended embargo. Please coordinate disclosure
with us while a fix is being prepared; you are free to disclose after 90 days
regardless of our progress. There is no bug bounty.

## Threat Model

This matters more than the version table, because the analyzer's job is to read
code that its operator may not trust.

### Untrusted input the analyzer is expected to survive

Analysed **source files** and **LLVM IR/bitcode** are treated as untrusted.
Crashes, hangs, unbounded memory growth, or memory-safety faults triggered by a
malformed or hostile `.c`/`.cpp`/`.ll`/`.bc` input are **in scope**.

### Trusted input — not a security boundary

The **compile database is configuration, not data**. Entries in
`compile_commands.json` — and `--compile-arg`, `-I`, `-D`, `--config` — are
handed to a real `clang::driver::Driver`, and the toolchain layer also spawns
processes via `llvm::sys::ExecuteAndWait`. A compile database therefore has the
same power as a compiler invocation: `-fplugin=`, `@response-file`, `-B`, and
similar flags lead to arbitrary code execution **by design**.

Consequently:

- Running the analyzer against a pull request from an untrusted fork **while
  using a compile database produced by that pull request** is equivalent to
  executing the contributor's code. Use `pull_request` (not
  `pull_request_target`), keep the workflow token read-only apart from
  `security-events: write`, and generate the compile database yourself.
- Reports of the form "a crafted `compile_commands.json` runs a command" are
  **out of scope** — that is the documented contract, and it is why this
  section exists.

### Also out of scope

- False positives and false negatives, including a missed defect in code you
  expected the analyzer to flag. These are correctness bugs — please file them
  as regular issues with a reproducer, they are taken seriously, but they are
  not handled as vulnerabilities.
- `fail-on` gate behaviour that lets a build pass. The gate reflects what was
  detected; it is not a security control.
- Results obtained from unsupported configurations (unsupported LLVM version,
  hand-edited cache directories, modified models under `models/`).
- Vulnerabilities in LLVM/Clang itself. Report those upstream; tell us too, so
  we can raise the minimum supported version.
