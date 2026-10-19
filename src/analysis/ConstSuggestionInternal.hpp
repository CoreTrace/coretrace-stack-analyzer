// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
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

    // A position in a file's original text: after the rewrite, a position in a rewritten file is
    // mapped back through the plan's edits (§5.4).
    struct SourcePosition
    {
        std::string file; // real path
        std::size_t offset = 0;
        bool operator==(const SourcePosition&) const = default;
        bool operator<(const SourcePosition& other) const
        {
            return std::tie(file, offset) < std::tie(other.file, other.offset);
        }
    };

    // Where a name is expanded and where it is written: one macro invocation can expand several
    // names at the same place (ruling 14).
    struct DeclarationSite
    {
        SourcePosition expansion;
        SourcePosition spelling;
        bool operator==(const DeclarationSite&) const = default;
        bool operator<(const DeclarationSite& other) const
        {
            return std::tie(expansion, spelling) < std::tie(other.expansion, other.spelling);
        }
    };

    enum class SourceLinkage : std::uint8_t
    {
        None,
        Internal,
        External
    };

    // The function a suggestion names, as the defining unit's initial compilation shows it (§3).
    struct EntityIdentity
    {
        std::vector<DeclarationSite> redeclarations;   // the declarations the plan rewrites, sorted
        std::string name;                              // as Clang prints it: dot, operator==, ~A
        std::optional<DeclarationSite> templateOrigin; // a specialization's pattern, first declared
        std::vector<std::string> symbols; // every variant's, before the rewrite (CodeGen)
        SourceLinkage linkage = SourceLinkage::None;
        std::uint8_t reservedPadding[7] = {};
    };

    // R, O and K (§5.4).
    enum class FactKind : std::uint8_t
    {
        Reference,
        Override,
        Constant
    };

    // Where a fact is: the instantiations around it, by their symbols, outermost first, and its
    // site.
    struct FactKey
    {
        std::vector<std::string> contexts;
        DeclarationSite site;
        bool operator==(const FactKey&) const = default;
        bool operator<(const FactKey& other) const
        {
            return std::tie(contexts, site) < std::tie(other.contexts, other.site);
        }
    };

    // What one compilation shows at a key, and what a reason names when it changes (Texts).
    struct Fact
    {
        FactKey key;
        // Sorted. R: the target's declaration sites, or the symbol of a specialization or an
        // implicit function (ruling 4); O: each overridden method, the same way; K: true, false
        // or the selected association.
        std::vector<std::string> value;
        std::string subject; // O: the method; K: _Generic for a selection
        std::string shown;   // the value, as a reason names it
        unsigned line = 0;   // the site's, where Clang expands it
        FactKind kind = FactKind::Reference;
        std::uint8_t reservedPadding[3] = {};
    };

    // The rewrite of one suggestion, planned on the defining unit's initial compilation (§5.5).
    struct SuggestionPlan
    {
        EntityIdentity entity;
        std::vector<DeclarationSite> parameters;        // the parameters it rewrites, sorted
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

    // The sites of one compilation's locations. After the rewrite, a position in a file that
    // `rewrittenBy` rewrites is mapped back to the original text through its edits.
    class SiteReader
    {
      public:
        SiteReader(const clang::SourceManager& sm, const SuggestionPlan* rewrittenBy);

        DeclarationSite operator()(clang::SourceLocation location);
        // The real path of a file, or its buffer's name when it is none.
        const std::string& fileOf(clang::FileID file);

      private:
        SourcePosition position(clang::SourceLocation location);

        const clang::SourceManager& sm_;
        std::map<std::string, std::vector<Edit>> edits_; // by real path, sorted by offset
        std::map<clang::FileID, std::string> files_;
    };

    // External for external or module linkage, internal for internal or unique-external.
    SourceLinkage linkageOf(const clang::FunctionDecl& function);

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

    // What to look for in one compilation: the function a suggestion names.
    struct Target
    {
        std::string symbol; // the suggestion's symbol, before the rewrite
        // Its plan; null in the defining unit's initial compilation, which plans it.
        const SuggestionPlan* plan = nullptr;
        // The parameter's position among those the source declares (P0), none for one it
        // does not declare.
        std::optional<unsigned> sourceIndex;
    };

    // An error or a checked warning, in the order Clang reports it.
    struct Diagnostic
    {
        std::string key; // place and kind, as `diagnostics` holds it
        std::string text;
        bool error = false;
        std::uint8_t reservedPadding[7] = {};
    };

    // What one compilation shows (§5.3, §5.6).
    struct UnitFacts
    {
        // About the function of one target.
        struct Entity
        {
            SuggestionPlan plan;              // in the compilation that plans
            std::vector<std::string> symbols; // every variant's, in this compilation
            std::string printedSignature;     // for __PRETTY_FUNCTION__
            std::string problem;
            std::vector<Fact> overrides; // O, for the methods named like it
            bool found = false;          // the compilation declares it
            // After the rewrite, a declaration in a rewritten file without the candidate.
            bool withoutCandidate = false;
            std::uint8_t reservedPadding[6] = {};
        };

        // A declaration that a rewritten file writes (§5.6).
        struct Written
        {
            std::string type; // written canonically
            std::string name;
            unsigned line = 0; // where Clang expands it
            std::uint8_t reservedPadding[4] = {};
        };

        bool timing = false;
        std::uint8_t reservedPadding[7] = {};
        std::string failure; // the compilation could not run
        std::multiset<std::string> diagnostics;
        std::vector<Diagnostic> reported;
        std::string firstDiagnostic;
        std::string fatal; // the first fatal error: the AST lacks the rest of the unit
        std::map<DeclarationSite, Written> writtenTypes; // in the rewritten files, by site
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
        std::vector<Fact> facts;                   // R and K, in the order of the traversal
        std::set<std::string> instantiations;      // the symbols of those around a fact
        std::vector<Entity> entities;              // indexed like the targets
        const_suggestion::MacroDefinitions macros; // read only when the compilation plans
    };

    // Compiles a unit and inspects it for `targets`. After the rewrite, `rewritten` remaps the
    // files that the plan `rewrittenBy` rewrites to their texts.
    UnitFacts inspect(const UnitCommand& command, const SuggestionPlan* rewrittenBy,
                      const std::map<std::string, std::string>& rewritten,
                      const std::vector<Target>& targets, std::string_view step, bool timing);

    // The files a unit reads and its identifiers, from its preprocessing.
    UnitScan scan(const UnitCommand& command, bool timing);
} // namespace ctrace::stack::analysis::const_suggestion
