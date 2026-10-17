// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

#include "analysis/ConstSuggestionCheck.hpp"

namespace clang
{
    class ASTContext;
    class FunctionDecl;
    class QualType;
} // namespace clang

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct PrintedSignature;
} // namespace ctrace::stack::analysis

namespace ctrace::stack::analysis::const_suggestion
{
    // What the check concludes about a suggestion, in one unit or overall (#217 v2, §3).
    enum class Verdict : std::uint8_t
    {
        Verified,
        Refuted,
        Indeterminate
    };

    struct Judgement
    {
        std::string reason; // empty when verified
        Verdict verdict = Verdict::Verified;
        std::uint8_t reservedPadding[7] = {};
    };

    // Refuted if a judgement refutes, else indeterminate if one is, else verified; with the
    // reason of the first judgement that has that verdict.
    Judgement aggregate(const std::vector<Judgement>& judgements);

    // 16 hexadecimal digits: xxh3 of the arguments and the working directory, each followed by a
    // NUL byte.
    std::string fingerprintOf(const UnitCommand& command);

    // A file's real path, or the path as given when it has none.
    std::string realPath(llvm::StringRef path);

    // `length` bytes at `offset` replaced with `text`.
    struct Edit
    {
        std::size_t offset = 0;
        std::size_t length = 0;
        std::string text;
    };

    // The rewrite of one suggestion, planned on the defining unit's initial compilation (§5.5).
    struct SuggestionPlan
    {
        std::map<std::string, std::vector<Edit>> edits; // by real path
        std::string expectedType; // the parameter's type once rewritten, written canonically
        std::optional<std::string> indeterminate; // why the plan is not executed
        unsigned sourceIndex = 0;
        // A free function with external linkage can be declared by a file that reads none of the
        // rewritten files; a method only where its class is defined. One with internal linkage
        // (static, anonymous namespace) cannot: another file's homonym is another function.
        bool declarableElsewhere = false;
        std::uint8_t reservedPadding[3] = {};
    };

    // A type written canonically, an anonymous type without its location.
    std::string writtenType(const clang::ASTContext& context, clang::QualType type);

    // Plans the rewrite of the parameter at `sourceIndex` of `function`, located in the defining
    // unit's initial compilation by the suggestion's symbol.
    SuggestionPlan plan(clang::ASTContext& context, const clang::FunctionDecl& function,
                        std::optional<unsigned> sourceIndex);

    // The demangled form of a symbol without const and spaces, with && written &: the rewritten
    // function, and what is named inside it, keep this form across the rewrite.
    std::string withoutConst(const std::string& symbol);

    // The constructor or destructor variant a symbol names, and whether it names a const method.
    std::string variantOf(const std::string& symbol);

    // The generated code of one module, ready to compare: what carries no meaning removed, and
    // each piece keyed by what it defines.
    struct ModuleText
    {
        std::map<std::string, std::string> globals; // functions, variables, aliases
        std::map<std::string, std::string> types;
        std::map<std::string, std::string> attributeGroups; // "#N" -> "{ ... }"
        std::map<std::string, std::string> metadata;        // "!N" -> its definition
        std::map<std::string, std::string> namedMetadata;   // "!name" -> its operands
        std::set<std::string> defined;                      // has code or data here
    };

    // Prepares `module`, which it changes (debug information and other metadata removed).
    ModuleText prepare(llvm::Module& module);

    // rewrittenCodeDifference, with the code before the rewrite already prepared.
    std::optional<std::string> codeDifference(const ModuleText& was, const ModuleText& now,
                                              llvm::StringRef rewritten,
                                              const PrintedSignature* signature,
                                              std::vector<std::string>* accepted,
                                              std::string* renamedTo);
} // namespace ctrace::stack::analysis::const_suggestion
