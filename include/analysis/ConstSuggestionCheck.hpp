// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "StackUsageAnalyzer.hpp"
#include "analysis/ConstParamAnalysis.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    // One input of the analysis, for the Clang check of const suggestions (#217).
    struct AnalyzedUnit
    {
        std::string filename; // as given to the analyzer
        const llvm::Module* module = nullptr;
        LanguageType language = LanguageType::Unknown;
        std::uint32_t reservedLanguagePadding = 0;
    };

    // What the preprocessing of one unit shows: the files it reads, and its identifiers (as
    // 64-bit hashes: a collision only adds a recheck).
    struct UnitScan
    {
        std::set<std::string> files;
        std::unordered_set<std::uint64_t> identifiers;
    };

    // How the check compiles one unit.
    struct UnitCommand
    {
        std::vector<std::string> args; // the driver's, as compilerlib builds them
        std::string workingDir;
    };

    // The inputs analyzed together.
    struct AnalyzedUnits
    {
        std::vector<AnalyzedUnit> units;
        // By path, found when first needed: the compile command of each unit (none when it has
        // none), and its scan.
        mutable std::mutex scansMutex;
        mutable std::map<std::string, std::optional<UnitCommand>> commands;
        mutable std::map<std::string, UnitScan> scans;
    };

    // The one input of an analysis that loads it alone.
    std::shared_ptr<const AnalyzedUnits> analyzedUnitOf(const std::string& filename,
                                                        const llvm::Module& module);

    // A symbol of a unit's compilation before a rewrite, and its counterpart after it, as the
    // compilation facts establish them (#217, §5.7).
    struct SymbolPair
    {
        std::string before;
        std::string after;
    };

    // What __PRETTY_FUNCTION__ prints for the rewritten function, before and after the rewrite.
    struct PrintedSignature
    {
        std::string before;
        std::string after;
    };

    // Compares the code generated for one unit before and after a rewrite. Returns the first
    // difference, or nullopt when the code matches once normalized:
    // - debug information is left out;
    // - metadata are compared by the order in which each function or global uses them, not by
    //   their numbers or contents;
    // - attribute groups are compared by contents, not by numbers;
    // - the symbols the rewrite renames are paired by `established`, the pairs that the rewrite's
    //   correspondences establish, and by them only: a symbol that only one side has, and that no
    //   pair covers, is a difference.
    // Two differences that a run can observe are accepted, each only when verified, and described
    // in `accepted`: the name of a type renamed with the function (_ZTS<T> holds the mangled name
    // of T), and a string constant whose only change is the function's printed signature, from
    // `signature` (null: none is accepted).
    std::optional<std::string> rewrittenCodeDifference(
        llvm::Module& before, llvm::Module& after, const std::vector<SymbolPair>& established,
        const PrintedSignature* signature = nullptr, std::vector<std::string>* accepted = nullptr);

    // Checks the suggestions made for `mod` with Clang (#217): rewrites every declaration of the
    // parameter with the suggested type, recompiles every analyzed unit that sees a rewritten file
    // or references the function, and compares their generated code. A suggestion is removed when
    // the rewrite adds an error or a tracked warning, or changes a call or reference target, an
    // override or a constant condition; it is kept as not checked when it cannot be shown valid,
    // a generated code difference included; the others are kept as checked. Each gets detail lines
    // that say what was checked, or why it could not be.
    void checkConstSuggestions(const llvm::Module& mod, std::vector<ConstParamIssue>& issues,
                               const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
