# CLAUDE.md

Guidance for Claude Code sessions working in this repository.

## Build and test

```sh
# macOS: add -DLLVM_DIR=$(brew --prefix llvm@20)/lib/cmake/llvm -DClang_DIR=$(brew --prefix llvm@20)/lib/cmake/clang
cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON
cmake --build build --target stack_usage_analyzer stack_usage_analyzer_unit_tests ownership_engine_unit_tests -j 8
./build/stack_usage_analyzer_unit_tests . && ./build/ownership_engine_unit_tests
python3 run_test.py --jobs 8 --no-cache   # needs a build with Z3, see README "SMT solver usage"
```

Format changed C/C++ files with clang-format 20, as CI does.

## Code-scanning comments on pull requests

Comments from `github-advanced-security[bot]` are code-scanning alerts. They come from SARIF
uploaded by two tools:

- `coretrace-stack-analyzer`: this repository's analyzer run on its own sources (`ci.yml`, step
  "Self-analysis", Linux job).
- `coretrace-concurrency-analyzer`: `concurrency.yml`.

Most alerts are `note` (Info) and do not fail CI. Handle them before a pull request is merged.

1. **List the comments.**

   ```sh
   gh api repos/CoreTrace/coretrace-stack-analyzer/pulls/<PR>/comments --paginate \
     -q '.[] | select(.user.login == "github-advanced-security[bot]") | "\(.path):\(.line // .original_line)"'
   ```

2. **Keep only the alerts the pull request introduces.** The pull-request ref lists every open
   alert of the repository. The new ones are those absent from `main`:

   ```sh
   R=CoreTrace/coretrace-stack-analyzer
   comm -23 \
     <(gh api "repos/$R/code-scanning/alerts?ref=refs/pull/<PR>/merge&state=open&per_page=100" --paginate -q '.[].number' | sort) \
     <(gh api "repos/$R/code-scanning/alerts?ref=refs/heads/main&state=open&per_page=100" --paginate -q '.[].number' | sort)
   ```

   A new alert whose rule and message match a `main` alert in the same function is that alert,
   moved by a line shift: nothing to do.

3. **Reproduce each remaining alert locally.**

   ```sh
   ./build/stack_usage_analyzer --compile-commands=build/compile_commands.json <file>
   ```

4. **Classify it.**
   - True positive: fix the code in the pull request, then check that step 3 no longer reports it.
   - False positive: do not rewrite correct code to silence the analyzer. Report it (below).

5. **Summarize the triage in the pull request:** the new alerts, how each was handled, and the
   listed alerts that already exist on `main`.

### Reporting a false positive

Opening an issue and dismissing an alert are public actions: confirm with the user first.

- For `coretrace-stack-analyzer`, open an issue in this repository labelled `bug`, titled
  `False positive: <rule id>`. Include the alert link, a minimal C/C++ reproducer, the command,
  the diagnostic, and why it is wrong. Then dismiss the alert, citing the issue:

  ```sh
  gh api -X PATCH repos/CoreTrace/coretrace-stack-analyzer/code-scanning/alerts/<alert> \
    -f state=dismissed -f dismissed_reason="false positive" -f dismissed_comment="<issue link>"
  ```

- For `coretrace-concurrency-analyzer`, the issue belongs to
  `CoreTrace/coretrace-concurrency-analyzer`.
