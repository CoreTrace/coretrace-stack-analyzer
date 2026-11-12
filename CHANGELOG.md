# Changelog

Notable changes are grouped by release. See [all releases](https://github.com/CoreTrace/coretrace-stack-analyzer/releases) for older versions.

## Unreleased

- An index-bounds pilot bounds `buf[i]` on a stack array, indexed directly rather than through
  a pointer, with ScalarEvolution. It works on a copy of the function whose local variables live
  in registers; the analyzed IR is unchanged. An index the copy proves in bounds no longer gets a
  report, such as `buf[i + 1]` in `for (i = 0; i < 10; i += 2)`. An index that leaves the array
  on every run of the function that returns is a Warning under the new rules
  `StackBufferOverflow.EveryRun` and `NegativeStackIndex.EveryRun`, and its message no longer
  says "potential". A range that only may leave the array is still a "potential" Warning, now
  also found when a parameter guarded before the loop sets its count. An access in a loop that
  runs no iteration gets the "this access appears unreachable" note. Where the copy does not
  bound the index, the existing result stays.
- Stack buffer overflow checks no longer take the store of an element's address, such as
  `char* end = &buf[10]` or the end of a range-based `for`, for a write to that element.
  Forming a pointer one past the end is valid C and C++. A read or write through a pointer
  variable holding the address is checked where it happens instead: `char* p = &buf[10];
  *p = 42;` is reported on `*p = 42`, with the variable in the alias path. The variable is
  followed when the store of the address is its last write on every path to the access; one
  assigned on a single branch is not.
- A call that receives the address of a stack array is checked against what the callee reads or
  writes through it, in the same file or in another file analyzed with it (#266):
  `char* p = &buf[8]; f(p);`, where `f` writes `*p` or passes `p` to a function that does, is
  reported on the call, with the bytes written and the place of the write. Only the accesses
  the callee makes on every run, at a constant offset from the address it receives, count. An
  access behind a condition, at a variable offset, or through a function pointer is not
  followed, and gives no report.
- VLA sizes, local stack sizes and the "this access appears unreachable" note on buffer
  overflows read a local variable from the store that last wrote it, as MemorySSA finds it, and
  apply casts to it. They used to take the last constant stored anywhere in the function. A
  store after the read, a store on one branch only, or a store of another width changed the
  result then, and `(unsigned char)300` read as 300. The note now also requires every edge into
  the access to be dead. It no longer appears on accesses that run, such as an access after a
  loop.
- Building requires LLVM 20. CMake rejects an older LLVM when it configures the build, where
  LLVM 19 passed that check and then failed to compile. On macOS, `scripts/build.sh` looks for
  Homebrew `llvm@20` before `llvm`, a later version, and no longer for `llvm@19` or `llvm@18`.
- The GitHub Action accepts `llvm-version: 20` only. On a runner for which apt.llvm.org
  publishes no LLVM 20 packages, such as `ubuntu-26.04`, it stops with an error before
  installing LLVM (#255).
- StackBufferOverflow and MultipleStoresToStackBuffer resolve each pointer variable to its
  stack array once per function, by strongly connected components: pointer variables
  assigned from one another no longer make the analysis walk every path between them,
  which never finished on zlib's `crc32.c` and cppcheck's `simplecpp.cpp` (#215). Pointers
  assigned from one another in a cycle that reaches two different arrays now resolve to no
  array, as any pointer with several possible arrays does, where each used to be reported
  against its own array.
- Added an authors file and linked the repository documents from the README.
- SARIF rules carry a title (`shortDescription`) and a description (`fullDescription`) instead
  of their id, and `describeRule` (`RuleCatalog.hpp`) gives them to programs linking the analyzer.
- With `--jobs` above 1, sources compile in parallel. Each compilation runs in its compile
  command's directory through coretrace-compiler's `-working-directory`, instead of moving the
  whole process there under a lock that let one source compile at a time. Compilations that pass
  `-mllvm`, `-mdebug-pass`, `-mlimit-float-precision`, `-ftime-report` or `-fno-integrated-cc1`
  still run one at a time. coretrace-compiler is pinned to a commit of
  CoreTrace/coretrace-compiler#262, which gives each compilation a working directory of its own.

## v0.22.0

- SMT queries support unknown ranges with wrapping semantics.
- SARIF rules include their CWE tags and security severity.
- Recursion diagnostics have named rules, and result paths preserve the input spelling.

[Full v0.22.0 notes](https://github.com/CoreTrace/coretrace-stack-analyzer/releases/tag/v0.22.0).
