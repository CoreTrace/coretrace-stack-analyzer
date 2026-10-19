// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <clang/Basic/SourceLocation.h>
#include <llvm/ADT/StringRef.h>

#include "analysis/ConstSuggestionCheck.hpp"

namespace clang
{
    class ASTContext;
    class FunctionDecl;
    class PPCallbacks;
    class QualType;
    class SourceManager;
} // namespace clang

namespace llvm
{
    class LLVMContext;
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

    // The macro definitions a compilation reads, by file: from each macro's name to its last
    // token, as offsets (D11).
    using MacroDefinitions = std::map<clang::FileID, std::vector<std::pair<unsigned, unsigned>>>;

    // Records them, in the compilation that plans.
    std::unique_ptr<clang::PPCallbacks> recordMacroDefinitions(const clang::SourceManager& sm,
                                                               MacroDefinitions& out);

    // Plans the rewrite of the parameter at `sourceIndex` of `function`, located in the defining
    // unit's initial compilation by the suggestion's symbol.
    SuggestionPlan plan(clang::ASTContext& context, const MacroDefinitions& macros,
                        const clang::FunctionDecl& function, std::optional<unsigned> sourceIndex);

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

    // What one compilation found about the function a suggestion names.
    struct FunctionFacts
    {
        // Before the rewrite, in the unit that defines it.
        SuggestionPlan plan;
        // In every compilation.
        std::string printedSignature;
        std::string problem;
        std::multiset<std::string> overrides;
    };

    // What to look for in one compilation.
    struct Target
    {
        std::string symbol;  // the function's symbol before the rewrite
        std::string renamed; // and after, once known
        std::string name;    // its identifier
        std::string expectedType;
        std::set<std::string> rewrittenFiles;
        const std::set<std::string>* symbolsBefore = nullptr; // to find the renamed function
        // The parameter's position among those the source declares (P0), none for one it
        // does not declare.
        std::optional<unsigned> sourceIndex;
        unsigned paramIndex = 0;
        bool plan = false; // before the rewrite, in the defining unit
        std::uint8_t reservedPadding[3] = {};
    };

    // An error or a checked warning, in the order Clang reports it.
    struct Diagnostic
    {
        std::string key; // place and kind, as `diagnostics` holds it
        std::string text;
        bool error = false;
        std::uint8_t reservedPadding[7] = {};
    };

    struct Compilation
    {
        bool timing = false;
        std::uint8_t reservedPadding[7] = {};
        std::string failure; // the compilation could not run
        std::multiset<std::string> diagnostics;
        std::vector<Diagnostic> reported;
        std::string firstDiagnostic;
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
        std::vector<FunctionFacts> facts;
        const_suggestion::MacroDefinitions macros; // read only when the compilation plans
    };

    // Compiles a unit, the files of `rewritten` remapped to their texts, and inspects it for
    // `targets`.
    Compilation compile(const UnitCommand& command,
                        const std::map<std::string, std::string>& rewritten,
                        std::vector<Target>& targets, std::string_view step, bool timing);

    // The files a unit reads and its identifiers, from its preprocessing.
    UnitScan scan(const UnitCommand& command, bool timing);
} // namespace ctrace::stack::analysis::const_suggestion
