# Changelog

Notable changes are grouped by release. See [all releases](https://github.com/CoreTrace/coretrace-stack-analyzer/releases) for older versions.

## Unreleased

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

## v0.22.0

- SMT queries support unknown ranges with wrapping semantics.
- SARIF rules include their CWE tags and security severity.
- Recursion diagnostics have named rules, and result paths preserve the input spelling.

[Full v0.22.0 notes](https://github.com/CoreTrace/coretrace-stack-analyzer/releases/tag/v0.22.0).
