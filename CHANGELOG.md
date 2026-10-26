# Changelog

Notable changes are grouped by release. See [all releases](https://github.com/CoreTrace/coretrace-stack-analyzer/releases) for older versions.

## Unreleased

- Added an authors file and linked the repository documents from the README.
- SARIF rules carry a title (`shortDescription`) and a description (`fullDescription`) instead
  of their id, and `describeRule` (`RuleCatalog.hpp`) gives them to programs linking the analyzer.

## v0.22.0

- SMT queries support unknown ranges with wrapping semantics.
- SARIF rules include their CWE tags and security severity.
- Recursion diagnostics have named rules, and result paths preserve the input spelling.

[Full v0.22.0 notes](https://github.com/CoreTrace/coretrace-stack-analyzer/releases/tag/v0.22.0).
