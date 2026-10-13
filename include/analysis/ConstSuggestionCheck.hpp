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

    // The inputs analyzed together.
    struct AnalyzedUnits
    {
        std::vector<AnalyzedUnit> units;
        // The scan of each unit, by path, found when first needed.
        mutable std::mutex scansMutex;
        mutable std::map<std::string, UnitScan> scans;
    };

    // The one input of an analysis that loads it alone.
    std::shared_ptr<const AnalyzedUnits> analyzedUnitOf(const std::string& filename,
                                                        const llvm::Module& module);

    // What __PRETTY_FUNCTION__ prints for the rewritten function, before and after the rewrite.
    struct PrintedSignature
    {
        std::string before;
        std::string after;
    };

    // Compares the code generated for one unit before and after the declarations of the function
    // `rewritten` (its symbol before the rewrite) are rewritten. Returns the first difference, or
    // nullopt when the code matches once normalized:
    // - debug information is left out;
    // - metadata are compared by the order in which each function or global uses them, not by
    //   their numbers or contents;
    // - attribute groups are compared by contents, not by numbers;
    // - the symbols the rewrite renames (the function, and what is named or instantiated inside
    //   it) are paired. A symbol the unit only declares is paired only if it is the function.
    // Two differences that a run can observe are accepted, each only when verified, and described
    // in `accepted`: the name of a type renamed with the function (_ZTS<T> holds the mangled name
    // of T), and a string constant whose only change is the function's printed signature, from
    // `signature` (null: none is accepted). `renamedTo` receives the function's symbol after the
    // rewrite.
    std::optional<std::string> rewrittenCodeDifference(llvm::Module& before, llvm::Module& after,
                                                       llvm::StringRef rewritten,
                                                       const PrintedSignature* signature = nullptr,
                                                       std::vector<std::string>* accepted = nullptr,
                                                       std::string* renamedTo = nullptr);

    // Checks the suggestions made for `mod` with Clang (#217): rewrites every declaration of the
    // parameter with the suggested type, recompiles every analyzed unit that sees a rewritten file
    // or references the function, and compares their generated code. A suggestion that does not
    // compile, or that changes the generated code, is removed; the others get detail lines that
    // say what was checked, or why it could not be.
    void checkConstSuggestions(const llvm::Module& mod, std::vector<ConstParamIssue>& issues,
                               const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
