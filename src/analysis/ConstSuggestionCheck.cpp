// SPDX-License-Identifier: Apache-2.0
// Checks the const suggestions of ConstParameterNotModified with Clang (#217): the analysis of the
// IR decides whether an execution writes the pointee; Clang decides whether the suggested type
// compiles in every analyzed file and leaves the generated code as it is.
#include "analysis/ConstSuggestionCheck.hpp"

#include "ConstSuggestionInternal.hpp"
#include "analysis/CompileCommands.hpp"
#include "analysis/InputPipeline.hpp"
#include "analyzer/HotspotProfiler.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticIDs.h>
#include <clang/Basic/SourceManager.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/CodeGen/ModuleBuilder.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <clang/Frontend/Utils.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <compilerlib/toolchain.hpp>
#include <coretrace/logger.hpp>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/xxhash.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::Judgement;
        using const_suggestion::variantOf;
        using const_suggestion::Verdict;
        using const_suggestion::withoutConst;

        // What the check covers: the analyzed files, or every file of the compilation database.
        constexpr const char* kCheckedAnalyzed =
            "checked in the analyzed files only, with their compile commands: ";
        constexpr const char* kCheckedDatabase =
            "checked in every file of the compilation database that sees the change, with its "
            "compile command: ";
        // What it verifies (README, "Const parameter suggestions checked with Clang").
        constexpr const char* kCheckedResult =
            "they compile with the suggested type, and their generated code matches, metadata "
            "included, once debug information, diagnostic metadata, descriptive names and "
            "renamed symbols are set aside";
        constexpr const char* kNotChecked = "not checked with Clang: ";
        constexpr const char* kDoesNotCompileAsItIs = "a file does not compile with Clang as it is";

        // Warnings that the C and C++ standards make constraint violations, or that a rewrite
        // turns on silently: the rewritten code must not add any.
        constexpr const char* kCheckedWarnings[] = {
            "incompatible-pointer-types-discards-qualifiers", "incompatible-pointer-types",
            "incompatible-function-pointer-types", "int-conversion", "overloaded-virtual"};

        // `length` bytes at `offset` replaced with `text`.
        struct Edit
        {
            std::size_t offset = 0;
            std::size_t length = 0;
            std::string text;
        };

        // What one compilation found about the function a suggestion names.
        struct FunctionFacts
        {
            // Before the rewrite, in the unit that defines it: the rewrite, and what it must give.
            std::map<std::string, std::vector<Edit>> edits; // by real path
            std::string notRewritten;
            std::string expectedType;
            // In every compilation.
            std::string printedSignature;
            std::string problem;
            std::multiset<std::string> overrides;
            unsigned paramIndex = 0;
            // A free function with external linkage can be declared by a file that reads none
            // of the rewritten files; a method only where its class is defined. One with internal
            // linkage (static, anonymous namespace) cannot: another file's homonym is another
            // function.
            bool declarableElsewhere = false;
            std::uint8_t reservedPadding[3] = {};
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
            unsigned debugArgNo = 0;
            unsigned paramIndex = 0;
            bool plan = false; // before the rewrite, in the defining unit
            std::uint8_t reservedPadding[7] = {};
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
        };

        // A unit before the rewrite, prepared once for every suggestion of the module: its
        // diagnostics, what the methods of each function's name override (indexed like the
        // targets), and its generated code ready to compare.
        struct Original
        {
            std::multiset<std::string> diagnostics;
            std::vector<std::multiset<std::string>> overrides;
            std::shared_ptr<const const_suggestion::ModuleText> code;
        };

        std::string realPath(llvm::StringRef path)
        {
            llvm::SmallString<256> out;
            if (llvm::sys::fs::real_path(path, out))
                return path.str();
            return out.str().str();
        }

        // Errors, and the checked warnings, by place and kind: their texts name types that the
        // rewrite changes.
        class DiagnosticCollector : public clang::DiagnosticConsumer
        {
          public:
            explicit DiagnosticCollector(Compilation& out) : out_(out) {}

            void HandleDiagnostic(clang::DiagnosticsEngine::Level level,
                                  const clang::Diagnostic& info) override
            {
                clang::DiagnosticConsumer::HandleDiagnostic(level, info);
                bool kept = level >= clang::DiagnosticsEngine::Error;
                if (level == clang::DiagnosticsEngine::Warning)
                {
                    const llvm::StringRef group =
                        info.getDiags()->getDiagnosticIDs()->getWarningOptionForDiag(info.getID());
                    kept = std::find(std::begin(kCheckedWarnings), std::end(kCheckedWarnings),
                                     group) != std::end(kCheckedWarnings);
                }
                if (!kept)
                    return;
                std::string place = "?";
                if (info.hasSourceManager() && info.getLocation().isValid())
                {
                    const clang::SourceManager& sm = info.getSourceManager();
                    const clang::PresumedLoc presumed =
                        sm.getPresumedLoc(sm.getExpansionLoc(info.getLocation()));
                    if (presumed.isValid())
                    {
                        place = realPath(presumed.getFilename()) + ":" +
                                std::to_string(presumed.getLine());
                    }
                }
                const std::string key = place + " #" + std::to_string(info.getID());
                out_.diagnostics.insert(key);
                llvm::SmallString<160> text;
                info.FormatDiagnostic(text);
                out_.reported.push_back({key, place + ": " + text.str().str(),
                                         level >= clang::DiagnosticsEngine::Error});
                if (out_.firstDiagnostic.empty())
                    out_.firstDiagnostic = out_.reported.back().text;
            }

          private:
            Compilation& out_;
        };

        // Whether `offset` of `buffer` lies on a #define line, continuations included.
        bool inMacroDefinition(llvm::StringRef buffer, std::size_t offset)
        {
            std::size_t start = buffer.rfind('\n', offset);
            start = start == llvm::StringRef::npos ? 0 : start + 1;
            // Whether the line before the one at `start` ends with a backslash, before its
            // line break, "\n" or "\r\n".
            const auto continued = [&](std::size_t at)
            {
                std::size_t end = at - 1;
                if (end > 0 && buffer[end - 1] == '\r')
                    --end;
                return end > 0 && buffer[end - 1] == '\\';
            };
            while (start >= 2 && continued(start))
            {
                const std::size_t previous = buffer.rfind('\n', start - 2);
                start = previous == llvm::StringRef::npos ? 0 : previous + 1;
            }
            const llvm::StringRef line = buffer.substr(start, offset - start).ltrim();
            return line.starts_with("#") && line.drop_front().ltrim().starts_with("define");
        }

        class Planner
        {
          public:
            Planner(clang::ASTContext& context, FunctionFacts& facts)
                : context_(context), sm_(context.getSourceManager()), facts_(facts)
            {
            }

            // Writes the suggested type where `param` writes its type.
            bool plan(const clang::FunctionDecl& function, const clang::ParmVarDecl& param)
            {
                param_ = &param;
                const clang::TypeSourceInfo* written = param.getTypeSourceInfo();
                if (!written)
                    return fail("the parameter's type is not written in the source");
                clang::TypeLoc loc = written->getTypeLoc();
                for (;;)
                {
                    loc = loc.getUnqualifiedLoc();
                    if (auto attributed = loc.getAs<clang::AttributedTypeLoc>())
                        loc = attributed.getModifiedLoc();
                    else if (auto paren = loc.getAs<clang::ParenTypeLoc>())
                        loc = paren.getInnerLoc();
                    else if (auto macro = loc.getAs<clang::MacroQualifiedTypeLoc>())
                        loc = macro.getInnerLoc();
                    else if (auto decayed = loc.getAs<clang::DecayedTypeLoc>())
                        loc = decayed.getOriginalLoc();
                    else
                        break;
                }
                if (auto pointer = loc.getAs<clang::PointerTypeLoc>())
                    return insertConst(function, pointer.getPointeeLoc().getBeginLoc());
                if (auto reference = loc.getAs<clang::LValueReferenceTypeLoc>())
                    return insertConst(function, reference.getPointeeLoc().getBeginLoc());
                if (auto reference = loc.getAs<clang::RValueReferenceTypeLoc>())
                {
                    // The suggestion names the const lvalue reference.
                    return insertConst(function, reference.getPointeeLoc().getBeginLoc()) &&
                           replace(function, reference.getAmpAmpLoc(), 2, "&");
                }
                if (auto array = loc.getAs<clang::ArrayTypeLoc>())
                    return insertConst(function, array.getElementLoc().getBeginLoc());
                // A typedef of a pointer: its desugared type, qualified.
                clang::TypeLoc named = loc;
                if (auto elaborated = loc.getAs<clang::ElaboratedTypeLoc>())
                    named = elaborated.getNamedTypeLoc();
                if (named.getAs<clang::TypedefTypeLoc>() || named.getAs<clang::UsingTypeLoc>())
                {
                    const auto* pointer = loc.getType()->getAs<clang::PointerType>();
                    if (!pointer || pointer->getPointeeType()->isFunctionType() ||
                        pointer->getPointeeType()->isArrayType())
                    {
                        return fail("the parameter's type is a typedef of no plain pointer");
                    }
                    clang::PrintingPolicy policy(context_.getLangOpts());
                    const std::string text =
                        pointer->getPointeeType().withConst().getAsString(policy) + " *";
                    if (text.find("(unnamed") != std::string::npos ||
                        text.find("(anonymous") != std::string::npos ||
                        text.find("(lambda") != std::string::npos)
                    {
                        return fail("the pointed type has no name to write");
                    }
                    const clang::SourceLocation begin = loc.getBeginLoc();
                    const clang::SourceLocation end = loc.getEndLoc();
                    if (begin.isMacroID() || end.isMacroID())
                        return fail("the parameter's typedef is written by a macro");
                    const unsigned length = sm_.getFileOffset(clang::Lexer::getLocForEndOfToken(
                                                end, 0, sm_, context_.getLangOpts())) -
                                            sm_.getFileOffset(begin);
                    return replace(function, begin, length, text);
                }
                return fail("the parameter's type has no form this check rewrites");
            }

          private:
            bool fail(std::string why)
            {
                if (facts_.notRewritten.empty())
                    facts_.notRewritten = std::move(why);
                return false;
            }

            bool insertConst(const clang::FunctionDecl& function, clang::SourceLocation at)
            {
                return replace(function, at, 0, "const ");
            }

            bool replace(const clang::FunctionDecl& function, clang::SourceLocation at,
                         unsigned length, const std::string& text)
            {
                if (at.isInvalid())
                    return fail("the parameter's type has no position");
                if (at.isMacroID())
                {
                    if (sm_.isMacroArgExpansion(at))
                    {
                        // Written in a macro argument: rewritten where it is written.
                        at = sm_.getSpellingLoc(at);
                    }
                    else
                    {
                        // Written by a macro: a macro is text, `const` goes before its
                        // invocation, unless that invocation writes the declaration too, or
                        // another parameter, which would take the qualifier.
                        const clang::CharSourceRange invocation = sm_.getExpansionRange(at);
                        const auto writes = [&](clang::SourceLocation place)
                        {
                            const clang::SourceLocation where = sm_.getExpansionLoc(place);
                            return !sm_.isBeforeInTranslationUnit(where, invocation.getBegin()) &&
                                   !sm_.isBeforeInTranslationUnit(invocation.getEnd(), where);
                        };
                        if (length != 0 || writes(function.getLocation()))
                            return fail("the declaration is written by a macro definition");
                        for (const clang::ParmVarDecl* other : function.parameters())
                        {
                            if (other != param_ && writes(other->getLocation()))
                            {
                                return fail("the parameter's type is written by a macro that "
                                            "writes other parameters too");
                            }
                        }
                        at = invocation.getBegin();
                    }
                    if (at.isMacroID())
                        return fail("the declaration is written by a macro definition");
                }
                const auto [file, offset] = sm_.getDecomposedLoc(at);
                const clang::OptionalFileEntryRef entry = sm_.getFileEntryRefForID(file);
                if (!entry)
                    return fail("the parameter's type is not written in a file");
                if (inMacroDefinition(sm_.getBufferData(file), offset))
                    return fail("the declaration is written by a macro definition");
                std::vector<Edit>& edits = facts_.edits[realPath(entry->getName())];
                const bool seen = std::any_of(edits.begin(), edits.end(), [&](const Edit& edit)
                                              { return edit.offset == offset; });
                if (!seen)
                    edits.push_back({offset, length, text});
                return true;
            }

            clang::ASTContext& context_;
            const clang::SourceManager& sm_;
            FunctionFacts& facts_;
            const clang::ParmVarDecl* param_ = nullptr;
        };

        // A type written canonically. An anonymous type, such as a lambda's, is written without
        // its location: the path of that location depends on how the file was named, relative
        // before the rewrite and absolute once the rewritten file is remapped.
        std::string writtenType(const clang::ASTContext& context, clang::QualType type)
        {
            clang::PrintingPolicy policy(context.getLangOpts());
            policy.AnonymousTagLocations = false;
            return type.getCanonicalType().getAsString(policy);
        }

        // The type the parameter must have once rewritten, written canonically.
        std::string expectedType(clang::ASTContext& context, clang::QualType type)
        {
            clang::QualType out;
            if (const auto* pointer = type->getAs<clang::PointerType>())
            {
                out = context.getQualifiedType(
                    context.getPointerType(pointer->getPointeeType().withConst()),
                    type.getQualifiers());
            }
            else if (type->isReferenceType())
            {
                out = context.getLValueReferenceType(type.getNonReferenceType().withConst());
            }
            else
            {
                return "";
            }
            return writtenType(context, out);
        }

        // Declarations of the function, explicit casts of its address, and what each method of
        // its name overrides.
        class Inspector : public clang::ASTConsumer, public clang::RecursiveASTVisitor<Inspector>
        {
          public:
            Inspector(clang::CodeGenAction& action, std::vector<Target>& targets, Compilation& out)
                : action_(action), targets_(targets), out_(out)
            {
            }

            bool shouldVisitTemplateInstantiations() const
            {
                return true;
            }

            void HandleTranslationUnit(clang::ASTContext& context) override
            {
                context_ = &context;
                out_.facts.resize(targets_.size());
                located_.assign(targets_.size(), nullptr);
                clang::CodeGenerator* generator = action_.getCodeGenerator();
                for (std::size_t i = 0; i < targets_.size(); ++i)
                    locate(i, generator);
                const analyzer::ScopedHotspot hotspot(out_.timing, "const_suggestion.inspect");
                TraverseDecl(context.getTranslationUnitDecl());
            }

            bool VisitFunctionDecl(clang::FunctionDecl* function)
            {
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    if (!isTarget(i, *function))
                        continue;
                    const Target& target = targets_[i];
                    FunctionFacts& facts = out_.facts[i];
                    if (!target.expectedType.empty() && facts.problem.empty() &&
                        !function->isDependentContext() &&
                        target.paramIndex < function->getNumParams())
                    {
                        const std::string type = writtenType(
                            *context_, function->getParamDecl(target.paramIndex)->getType());
                        if (type != target.expectedType)
                        {
                            const clang::SourceManager& sm = context_->getSourceManager();
                            const clang::SourceLocation at =
                                sm.getExpansionLoc(function->getLocation());
                            const clang::OptionalFileEntryRef entry =
                                sm.getFileEntryRefForID(sm.getFileID(at));
                            const std::string file = entry ? realPath(entry->getName()) : "";
                            facts.problem =
                                target.rewrittenFiles.count(file)
                                    ? "rewritten:" + type
                                    : "declared in " + llvm::sys::path::filename(file).str() + ":" +
                                          std::to_string(sm.getExpansionLineNumber(at)) +
                                          " with type " + type;
                        }
                    }
                    if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function))
                        recordOverrides(facts, *method);
                }
                if (const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function))
                {
                    for (std::size_t i = 0; i < targets_.size(); ++i)
                    {
                        if (method->getIdentifier() &&
                            method->getIdentifier()->getName() == targets_[i].name &&
                            !isTarget(i, *function))
                        {
                            recordOverrides(out_.facts[i], *method);
                        }
                    }
                }
                return true;
            }

            bool VisitExplicitCastExpr(clang::ExplicitCastExpr* cast)
            {
                // `(void)f` converts the function to no other type.
                if (cast->getType()->isVoidType())
                    return true;
                const clang::Expr* operand = cast->getSubExpr()->IgnoreParenImpCasts();
                if (const auto* address = llvm::dyn_cast<clang::UnaryOperator>(operand))
                {
                    if (address->getOpcode() == clang::UO_AddrOf)
                        operand = address->getSubExpr()->IgnoreParenImpCasts();
                }
                const auto* reference = llvm::dyn_cast<clang::DeclRefExpr>(operand);
                const auto* function =
                    reference ? llvm::dyn_cast<clang::FunctionDecl>(reference->getDecl()) : nullptr;
                if (!function)
                    return true;
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    if (out_.facts[i].problem.empty() && isTarget(i, *function))
                        out_.facts[i].problem = "its address is converted by an explicit cast";
                }
                return true;
            }

          private:
            void recordOverrides(FunctionFacts& facts, const clang::CXXMethodDecl& method)
            {
                std::vector<std::string> overridden;
                for (const clang::CXXMethodDecl* base : method.overridden_methods())
                    overridden.push_back(base->getQualifiedNameAsString());
                std::sort(overridden.begin(), overridden.end());
                facts.overrides.insert(method.getQualifiedNameAsString() + " -> " +
                                       llvm::join(overridden, ", "));
            }

            // The symbol CodeGen gives the function: it numbers unnamed types, such as a lambda
            // ($_0, $_1), in the order it mangles them, as the IR names them.
            std::string symbolOf(const clang::FunctionDecl& function)
            {
                clang::CodeGenerator* generator = action_.getCodeGenerator();
                if (!generator || function.isDependentContext() ||
                    llvm::isa<clang::CXXConstructorDecl>(function) ||
                    llvm::isa<clang::CXXDestructorDecl>(function))
                {
                    return "";
                }
                return generator->GetMangledName(clang::GlobalDecl(&function)).str();
            }

            // Whether `function` is a declaration of the function target i names, in this
            // compilation: the one CodeGen emitted under its symbol, a redeclaration of it, or a
            // declaration with its symbol before or after the rewrite.
            bool isTarget(std::size_t i, const clang::FunctionDecl& function)
            {
                const clang::FunctionDecl* found = located_[i];
                if (found && function.getCanonicalDecl() == found->getCanonicalDecl())
                    return true;
                const Target& target = targets_[i];
                if (!function.getIdentifier() || function.getIdentifier()->getName() != target.name)
                    return false;
                const std::string symbol = symbolOf(function);
                return !symbol.empty() && (symbol == target.symbol || symbol == target.renamed);
            }

            void locate(std::size_t i, clang::CodeGenerator* generator)
            {
                Target& target = targets_[i];
                FunctionFacts& facts = out_.facts[i];
                if (!generator)
                    return;
                std::string symbol = target.symbol;
                if (!target.plan && target.symbolsBefore && generator->GetModule())
                {
                    // After the rewrite, the function may carry a new symbol: the one this
                    // compilation defines that it did not before, with the same name once const
                    // is left out.
                    const std::string key = withoutConst(target.symbol);
                    const std::string variant = variantOf(target.symbol);
                    for (const llvm::Function& emitted : *generator->GetModule())
                    {
                        const std::string name = emitted.getName().str();
                        if (!emitted.isDeclaration() && !target.symbolsBefore->count(name) &&
                            withoutConst(name) == key && variantOf(name) == variant)
                        {
                            symbol = name;
                        }
                    }
                    if (target.renamed.empty())
                        target.renamed = symbol;
                }
                const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(
                    generator->GetDeclForMangledName(symbol));
                located_[i] = function;
                if (!function)
                    return;
                facts.printedSignature = clang::PredefinedExpr::ComputeName(
                    clang::PredefinedIdentKind::PrettyFunction, function);
                if (target.name.empty() && function->getIdentifier())
                    target.name = function->getIdentifier()->getName().str();
                if (target.plan)
                    plan(target, facts, *function);
            }

            void plan(const Target& target, FunctionFacts& facts,
                      const clang::FunctionDecl& function)
            {
                const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
                facts.declarableElsewhere = method == nullptr && function.isExternallyVisible();
                // An explicit object parameter (C++23) is numbered like the others.
                const unsigned implicitThis =
                    method && method->isImplicitObjectMemberFunction() ? 1 : 0;
                if (target.debugArgNo <= implicitThis)
                {
                    facts.notRewritten = "the parameter is not found in the source";
                    return;
                }
                facts.paramIndex = target.debugArgNo - 1 - implicitThis;
                if (facts.paramIndex >= function.getNumParams())
                {
                    facts.notRewritten = "the parameter is not found in the source";
                    return;
                }
                facts.expectedType =
                    expectedType(*context_, function.getParamDecl(facts.paramIndex)->getType());
                if (facts.expectedType.empty())
                {
                    facts.notRewritten = "the parameter is no pointer or reference";
                    return;
                }
                const clang::FunctionDecl* pattern = function.getTemplateInstantiationPattern();
                if (!pattern)
                    pattern = &function;
                Planner planner(*context_, facts);
                for (const clang::FunctionDecl* declaration : pattern->redecls())
                {
                    if (declaration->isImplicit())
                        continue;
                    if (facts.paramIndex >= declaration->getNumParams())
                    {
                        facts.notRewritten = "the declarations disagree on the parameters";
                        return;
                    }
                    if (!planner.plan(*declaration, *declaration->getParamDecl(facts.paramIndex)))
                        return;
                }
            }

            std::uint8_t reservedPadding_[7] = {};
            clang::CodeGenAction& action_;
            std::vector<Target>& targets_;
            Compilation& out_;
            clang::ASTContext* context_ = nullptr;
            std::vector<const clang::FunctionDecl*> located_;
        };

        class InspectAction : public clang::EmitLLVMOnlyAction
        {
          public:
            InspectAction(llvm::LLVMContext* context, std::vector<Target>& targets,
                          Compilation& out)
                : clang::EmitLLVMOnlyAction(context), targets_(targets), out_(out)
            {
            }

          protected:
            std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& ci,
                                                                  llvm::StringRef file) override
            {
                std::unique_ptr<clang::ASTConsumer> generator =
                    clang::EmitLLVMOnlyAction::CreateASTConsumer(ci, file);
                if (!generator)
                    return nullptr;
                std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
                consumers.push_back(std::move(generator));
                consumers.push_back(std::make_unique<Inspector>(*this, targets_, out_));
                return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
            }

          private:
            std::vector<Target>& targets_;
            Compilation& out_;
        };

        bool commandFor(const AnalyzedUnit& unit, const AnalysisConfig& config, UnitCommand& out,
                        std::string& error)
        {
            std::vector<std::string> args;
            if (!buildSourceCompileCommand(unit.filename, unit.language, config, args,
                                           out.workingDir, error))
            {
                return false;
            }
            // Options only compilerlib understands.
            std::erase_if(args, [](const std::string& arg) { return arg.starts_with("--ct-"); });
            compilerlib::DriverConfig driver;
            if (!compilerlib::resolveDriverConfig(args, driver, error))
                return false;
            out.args = {driver.clang_path};
            if (driver.force_cxx_driver)
                out.args.push_back("--driver-mode=g++");
            if (driver.add_resource_dir)
            {
                out.args.push_back("-resource-dir");
                out.args.push_back(driver.resource_dir);
            }
            if (driver.add_sysroot)
            {
                out.args.push_back("-isysroot");
                out.args.push_back(driver.sysroot);
            }
            out.args.insert(out.args.end(), args.begin(), args.end());
            return true;
        }

        Compilation compile(const UnitCommand& command,
                            const std::map<std::string, std::string>& rewritten,
                            std::vector<Target>& targets, std::string_view step, bool timing)
        {
            const analyzer::ScopedHotspot hotspot(timing, step);
            static std::once_flag targetsReady;
            std::call_once(targetsReady,
                           []
                           {
                               llvm::InitializeAllTargetInfos();
                               llvm::InitializeAllTargets();
                               llvm::InitializeAllTargetMCs();
                           });
            Compilation out;
            out.timing = timing;
            std::string error;
            const bool ran = runSerializedCompile(
                command.workingDir,
                [&]
                {
                    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> files =
                        llvm::vfs::getRealFileSystem();
                    DiagnosticCollector collector(out);
                    std::vector<const char*> argv;
                    for (const std::string& arg : command.args)
                        argv.push_back(arg.c_str());
                    clang::CreateInvocationOptions options;
                    options.Diags = clang::CompilerInstance::createDiagnostics(
                        *files, new clang::DiagnosticOptions(), &collector, false);
                    options.VFS = files;
                    options.ProbePrecompiled = false;
                    std::unique_ptr<clang::CompilerInvocation> invocation =
                        clang::createInvocation(argv, options);
                    if (!invocation)
                    {
                        out.failure =
                            "the compile command is not understood: " + out.firstDiagnostic;
                        return;
                    }
                    // Nothing is written: no dependency file, no output.
                    invocation->getDependencyOutputOpts() = clang::DependencyOutputOptions();
                    // The inspection reads the AST after CodeGen: keep it for the backend.
                    invocation->getCodeGenOpts().ClearASTBeforeBackend = false;
                    // Values and blocks numbered, not named: a block that inlining names after
                    // the inlined function carries the rewritten function's symbol.
                    invocation->getCodeGenOpts().DiscardValueNames = true;
                    // The comparison removes debug information: do not generate it.
                    invocation->getCodeGenOpts().setDebugInfo(llvm::codegenoptions::NoDebugInfo);
                    clang::DiagnosticOptions& diagnostics = invocation->getDiagnosticOpts();
                    // No "N warnings generated" on the analyzer's output.
                    diagnostics.ShowCarets = false;
                    diagnostics.IgnoreWarnings = false;
                    for (const char* warning : kCheckedWarnings)
                        diagnostics.Warnings.push_back(warning);
                    out.diagnostics.clear();
                    out.reported.clear();
                    out.firstDiagnostic.clear();

                    clang::CompilerInstance instance;
                    instance.setInvocation(std::move(invocation));
                    instance.createDiagnostics(*files, &collector, false);
                    instance.createFileManager(files);
                    for (const auto& [path, text] : rewritten)
                    {
                        instance.getPreprocessorOpts().addRemappedFile(
                            path, llvm::MemoryBuffer::getMemBufferCopy(text, path).release());
                    }
                    instance.getPreprocessorOpts().RetainRemappedFileBuffers = false;
                    instance.getFrontendOpts().DisableFree = false;
                    instance.getCodeGenOpts().DisableFree = false;
                    out.context = std::make_unique<llvm::LLVMContext>();
                    InspectAction action(out.context.get(), targets, out);
                    instance.ExecuteAction(action);
                    out.module = action.takeModule();
                },
                error);
            if (!ran)
                out.failure = error;
            return out;
        }

        // Preprocesses a unit and records every identifier, after macro expansion.
        class ScanAction : public clang::PreprocessorFrontendAction
        {
          public:
            explicit ScanAction(std::unordered_set<std::uint64_t>& identifiers)
                : identifiers_(identifiers)
            {
            }

          protected:
            void ExecuteAction() override
            {
                clang::Preprocessor& preprocessor = getCompilerInstance().getPreprocessor();
                preprocessor.EnterMainSourceFile();
                clang::Token token;
                do
                {
                    preprocessor.Lex(token);
                    if (token.is(clang::tok::identifier))
                        identifiers_.insert(
                            llvm::xxh3_64bits(token.getIdentifierInfo()->getName()));
                } while (token.isNot(clang::tok::eof));
            }

          private:
            std::unordered_set<std::uint64_t>& identifiers_;
        };

        // The files a unit reads and its identifiers, from its preprocessing. Empty when the
        // unit cannot be preprocessed without error: what it reads is then unknown.
        UnitScan scan(const UnitCommand& command, bool timing)
        {
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.preprocess");
            UnitScan out;
            std::string error;
            runSerializedCompile(
                command.workingDir,
                [&]
                {
                    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> files =
                        llvm::vfs::getRealFileSystem();
                    clang::IgnoringDiagConsumer ignore;
                    std::vector<const char*> argv;
                    for (const std::string& arg : command.args)
                        argv.push_back(arg.c_str());
                    clang::CreateInvocationOptions options;
                    options.Diags = clang::CompilerInstance::createDiagnostics(
                        *files, new clang::DiagnosticOptions(), &ignore, false);
                    options.VFS = files;
                    options.ProbePrecompiled = false;
                    std::unique_ptr<clang::CompilerInvocation> invocation =
                        clang::createInvocation(argv, options);
                    if (!invocation)
                        return;
                    invocation->getDependencyOutputOpts() = clang::DependencyOutputOptions();
                    invocation->getDiagnosticOpts().ShowCarets = false;
                    clang::CompilerInstance instance;
                    instance.setInvocation(std::move(invocation));
                    instance.createDiagnostics(*files, &ignore, false);
                    instance.createFileManager(files);
                    ScanAction action(out.identifiers);
                    instance.ExecuteAction(action);
                    if (!instance.hasSourceManager() ||
                        instance.getDiagnostics().hasErrorOccurred())
                        return;
                    const clang::SourceManager& sm = instance.getSourceManager();
                    for (auto it = sm.fileinfo_begin(); it != sm.fileinfo_end(); ++it)
                        out.files.insert(realPath(it->first.getName()));
                },
                error);
            return out;
        }

        // An input as a real absolute path: relative inputs resolve from the input directory.
        std::string canonicalPath(const std::string& filename, const AnalysisConfig& config)
        {
            llvm::SmallString<256> path(filename);
            if (!llvm::sys::path::is_absolute(path))
            {
                llvm::SmallString<256> base(config.inputBaseDir);
                if (base.empty())
                    llvm::sys::fs::current_path(base);
                llvm::sys::path::append(base, filename);
                path = base;
            }
            return realPath(path);
        }

        bool references(const llvm::Module& module, llvm::StringRef symbol)
        {
            const llvm::GlobalValue* value = module.getNamedValue(symbol);
            return value && !value->hasLocalLinkage();
        }

        std::map<std::string, std::string> rewrite(const FunctionFacts& facts)
        {
            std::map<std::string, std::string> out;
            for (const auto& [path, edits] : facts.edits)
            {
                llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
                    llvm::MemoryBuffer::getFile(path);
                if (!buffer)
                    continue;
                std::string text = (*buffer)->getBuffer().str();
                std::vector<Edit> sorted = edits;
                std::sort(sorted.begin(), sorted.end(),
                          [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
                for (const Edit& edit : sorted)
                    text.replace(edit.offset, edit.length, edit.text);
                out[path] = std::move(text);
            }
            return out;
        }

        // The overrides that each target's inspection recorded, indexed like the targets.
        std::vector<std::multiset<std::string>> overridesOf(const Compilation& compilation)
        {
            std::vector<std::multiset<std::string>> out;
            for (const FunctionFacts& facts : compilation.facts)
                out.push_back(facts.overrides);
            return out;
        }

        // A unit before the rewrite, by its file and its configuration: compiled once for every
        // suggestion of the module (#217, §5, step 3).
        using OriginalCache = std::map<std::string, std::optional<Original>>;

        const std::optional<Original>& originalOf(const UnitCommand& command,
                                                  const std::string& key,
                                                  const std::vector<Target>& planned,
                                                  OriginalCache& cache, bool timing)
        {
            std::optional<Original>& out =
                cache[key + '\0' + const_suggestion::fingerprintOf(command)];
            if (out)
                return out;
            std::vector<Target> targets = planned;
            for (Target& target : targets)
            {
                target.plan = false;
                target.expectedType.clear();
            }
            Compilation before =
                compile(command, {}, targets, "const_suggestion.compile.other_before", timing);
            if (!before.failure.empty() || !before.module)
                return out;
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.prepare");
            out = Original{before.diagnostics, overridesOf(before),
                           std::make_shared<const_suggestion::ModuleText>(
                               const_suggestion::prepare(*before.module))};
            return out;
        }

        Judgement refuted(std::string why)
        {
            return {std::move(why), Verdict::Refuted};
        }

        Judgement indeterminate(std::string why)
        {
            return {std::move(why), Verdict::Indeterminate};
        }

        // Checks one suggestion, the target at `index`, in one unit, against the unit before the
        // rewrite.
        Judgement checkUnit(const UnitCommand& command, const std::optional<Original>& before,
                            std::size_t index, const Target& planned,
                            const std::map<std::string, std::string>& rewritten,
                            const std::set<std::string>& symbolsBefore, const FunctionFacts& plan,
                            PrintedSignature& signature, std::vector<std::string>& accepted,
                            std::string& renamed, bool timing)
        {
            if (!before)
                return indeterminate(kDoesNotCompileAsItIs);
            std::vector<Target> targets{planned};
            targets[0].plan = false;
            targets[0].renamed = renamed;
            targets[0].expectedType = plan.expectedType;
            targets[0].paramIndex = plan.paramIndex;
            targets[0].symbolsBefore = &symbolsBefore;
            for (const auto& [path, edits] : plan.edits)
                targets[0].rewrittenFiles.insert(path);
            Compilation after =
                compile(command, rewritten, targets, "const_suggestion.compile.after", timing);
            if (!after.failure.empty())
                return indeterminate(after.failure);
            // The first diagnostic the rewrite adds, not one the unit had before it.
            for (const Diagnostic& diagnostic : after.reported)
            {
                if (after.diagnostics.count(diagnostic.key) >
                    before->diagnostics.count(diagnostic.key))
                {
                    return refuted(
                        (diagnostic.error ? "does not compile: " : "gains a checked warning: ") +
                        diagnostic.text);
                }
            }
            if (!after.module)
                return refuted("does not compile: " + after.firstDiagnostic);
            if (renamed.empty())
                renamed = targets[0].renamed;
            const FunctionFacts& facts = after.facts.empty() ? FunctionFacts() : after.facts[0];
            if (facts.problem.starts_with("rewritten:"))
                return indeterminate("the rewritten declaration does not get the suggested type");
            if (!facts.problem.empty())
                return refuted(facts.problem);
            if (before->overrides[index] != facts.overrides)
                return refuted("the methods it overrides change");
            if (signature.after.empty() && !facts.printedSignature.empty())
                signature.after = facts.printedSignature;
            std::string renamedTo;
            std::optional<std::string> difference;
            {
                const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.compare");
                const const_suggestion::ModuleText now = const_suggestion::prepare(*after.module);
                difference = const_suggestion::codeDifference(*before->code, now, planned.symbol,
                                                              &signature, &accepted, &renamedTo);
            }
            if (difference)
                return refuted("the generated code changes: " + *difference);
            if (renamed.empty() || renamed == planned.symbol)
                renamed = renamedTo;
            return {};
        }
    } // namespace

    std::string const_suggestion::fingerprintOf(const UnitCommand& command)
    {
        std::string text;
        for (const std::string& arg : command.args)
            text.append(arg).push_back('\0');
        text.append(command.workingDir).push_back('\0');
        return llvm::utohexstr(llvm::xxh3_64bits(text), /*LowerCase=*/true, /*Width=*/16);
    }

    Judgement const_suggestion::aggregate(const std::vector<Judgement>& judgements)
    {
        for (const Verdict verdict : {Verdict::Refuted, Verdict::Indeterminate})
        {
            for (const Judgement& judgement : judgements)
            {
                if (judgement.verdict == verdict)
                    return judgement;
            }
        }
        return {};
    }

    std::shared_ptr<const AnalyzedUnits> analyzedUnitOf(const std::string& filename,
                                                        const llvm::Module& module)
    {
        auto units = std::make_shared<AnalyzedUnits>();
        units->units.push_back({filename, &module, detectFromExtension(filename)});
        return units;
    }

    void checkConstSuggestions(const llvm::Module& mod, std::vector<ConstParamIssue>& issues,
                               const AnalysisConfig& config)
    {
        if (issues.empty() || config.warningsOnly || !config.constSuggestionCheck)
            return;
        const bool timing = config.timing;
        const analyzer::ScopedHotspot total(timing, "const_suggestion.total");
        const AnalyzedUnits* units = config.analyzedUnits.get();
        const AnalyzedUnit* self = nullptr;
        for (std::size_t i = 0; units && i < units->units.size(); ++i)
        {
            if (units->units[i].module == &mod)
                self = &units->units[i];
        }
        const auto trace = [](const ConstParamIssue& issue, const std::string& outcome)
        {
            coretrace::log(coretrace::Level::Debug, "[const-suggestion] {} parameter {}: {}\n",
                           issue.funcName, issue.binding.name, outcome);
        };
        // Each unit judged, by its file and its configuration, after T v1's line.
        const auto traceUnits = [](const ConstParamIssue& issue,
                                   const std::vector<std::string>& judged,
                                   const std::vector<Judgement>& judgements)
        {
            static constexpr const char* kVerdicts[] = {"verified", "refuted", "indeterminate"};
            for (std::size_t k = 0; k < judgements.size(); ++k)
            {
                const Judgement& judgement = judgements[k];
                coretrace::log(coretrace::Level::Debug,
                               "[const-suggestion] {} parameter {}, unit {}: {}{}\n",
                               issue.funcName, issue.binding.name, judged[k],
                               kVerdicts[static_cast<std::size_t>(judgement.verdict)],
                               judgement.reason.empty() ? "" : ": " + judgement.reason);
            }
        };
        const auto label = [](const std::string& file, const UnitCommand* command)
        {
            return file + " [" + (command ? const_suggestion::fingerprintOf(*command) : "none") +
                   "]";
        };
        const auto notChecked = [&](const std::string& why)
        {
            for (ConstParamIssue& issue : issues)
            {
                issue.checkLines.push_back(kNotChecked + why);
                trace(issue, "not checked, " + why);
            }
        };
        if (!self || (self->language != LanguageType::C && self->language != LanguageType::CXX))
        {
            notChecked("no source file, so whether the suggested type compiles was not verified");
            return;
        }
        UnitCommand command;
        std::string error;
        if (!commandFor(*self, config, command, error))
        {
            notChecked("no compile command (" + error + ")");
            return;
        }

        // Before the rewrite, the defining unit: one compilation plans every suggestion.
        std::vector<Target> targets(issues.size());
        for (std::size_t i = 0; i < issues.size(); ++i)
        {
            targets[i].symbol = issues[i].funcName;
            targets[i].debugArgNo = issues[i].debugArgNo;
            targets[i].plan = true;
        }
        Compilation before =
            compile(command, {}, targets, "const_suggestion.compile.defining_before", timing);
        if (!before.failure.empty() || !before.module || before.facts.size() != issues.size())
        {
            notChecked("the file does not compile with Clang as it is");
            return;
        }
        std::set<std::string> symbolsBefore;
        for (const llvm::Function& function : *before.module)
            symbolsBefore.insert(function.getName().str());
        std::shared_ptr<const const_suggestion::ModuleText> definingCode;
        {
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.prepare");
            definingCode = std::make_shared<const_suggestion::ModuleText>(
                const_suggestion::prepare(*before.module));
        }
        const std::optional<Original> definingBefore =
            Original{before.diagnostics, overridesOf(before), definingCode};
        OriginalCache originals;

        // The other units a rewrite may reach: the analyzed ones and, with a compilation
        // database, every C or C++ file it lists.
        std::vector<AnalyzedUnit> candidates;
        std::set<std::string> analyzed;
        for (const AnalyzedUnit& unit : units->units)
        {
            analyzed.insert(canonicalPath(unit.filename, config));
            if (&unit != self)
                candidates.push_back(unit);
        }
        if (config.compilationDatabase)
        {
            for (const std::string& file : config.compilationDatabase->listSourceFiles())
            {
                const LanguageType language = detectFromExtension(file);
                if ((language == LanguageType::C || language == LanguageType::CXX) &&
                    !analyzed.count(canonicalPath(file, config)))
                {
                    candidates.push_back({file, nullptr, language});
                }
            }
        }

        std::vector<ConstParamIssue> kept;
        for (std::size_t i = 0; i < issues.size(); ++i)
        {
            ConstParamIssue& issue = issues[i];
            const FunctionFacts& plan = before.facts[i];
            if (plan.printedSignature.empty() && plan.notRewritten.empty())
            {
                issue.checkLines.push_back(kNotChecked +
                                           std::string("the function is not found in the source"));
                trace(issue, "not checked, the function is not found in the source");
                kept.push_back(std::move(issue));
                continue;
            }
            if (!plan.notRewritten.empty())
            {
                issue.checkLines.push_back(kNotChecked + plan.notRewritten);
                trace(issue, "not checked, " + plan.notRewritten);
                kept.push_back(std::move(issue));
                continue;
            }
            const std::map<std::string, std::string> rewritten = rewrite(plan);
            std::set<std::string> rewrittenFiles;
            for (const auto& [path, text] : rewritten)
                rewrittenFiles.insert(path);

            // The defining unit first: it gives the function's symbol after the rewrite.
            Target planned = targets[i];
            planned.plan = false;
            PrintedSignature signature{plan.printedSignature, ""};
            std::vector<std::string> accepted;
            std::string renamed;
            std::vector<Judgement> judgements{checkUnit(command, definingBefore, i, planned,
                                                        rewritten, symbolsBefore, plan, signature,
                                                        accepted, renamed, timing)};
            std::vector<std::string> judged{label(canonicalPath(self->filename, config), &command)};
            // Then every other unit that sees a rewritten file or references the function: the
            // analyzed ones and, with a compilation database, every file it lists, analyzed or
            // not. One that cannot be checked leaves the suggestion unchecked, unless another
            // one refutes it.
            const auto cannotCheck =
                [&](const std::string& unit, const UnitCommand* known, const std::string& why)
            {
                judged.push_back(label(unit, known));
                judgements.push_back(indeterminate(why));
            };
            for (const AnalyzedUnit& unit : candidates)
            {
                if (judgements.front().verdict != Verdict::Verified ||
                    judgements.back().verdict == Verdict::Refuted)
                {
                    break;
                }
                const bool referencing = unit.module && references(*unit.module, issue.funcName);
                if (unit.language != LanguageType::C && unit.language != LanguageType::CXX)
                {
                    if (referencing)
                    {
                        cannotCheck(canonicalPath(unit.filename, config), nullptr,
                                    unit.filename + " uses the function but has no source");
                    }
                    continue;
                }
                const std::string key = canonicalPath(unit.filename, config);
                std::optional<UnitCommand> known;
                bool resolved = false;
                {
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    const auto found = units->commands.find(key);
                    if (found != units->commands.end())
                    {
                        known = found->second;
                        resolved = true;
                    }
                }
                if (!resolved)
                {
                    UnitCommand command;
                    if (commandFor(unit, config, command, error))
                        known = std::move(command);
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    units->commands[key] = known;
                }
                if (!known)
                {
                    cannotCheck(key, nullptr, "no compile command for " + unit.filename);
                    continue;
                }
                const UnitCommand& other = *known;
                UnitScan seen;
                bool cached = false;
                {
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    const auto found = units->scans.find(key);
                    if (found != units->scans.end())
                    {
                        seen = found->second;
                        cached = true;
                    }
                }
                if (!cached)
                {
                    seen = scan(other, timing);
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    units->scans[key] = seen;
                }
                if (seen.files.empty())
                {
                    cannotCheck(key, &other, unit.filename + " cannot be preprocessed with Clang");
                    continue;
                }
                const bool sees =
                    referencing ||
                    std::any_of(rewrittenFiles.begin(), rewrittenFiles.end(),
                                [&](const std::string& path) { return seen.files.count(path); }) ||
                    (plan.declarableElsewhere &&
                     seen.identifiers.count(llvm::xxh3_64bits(planned.name)) != 0);
                if (!sees)
                    continue;
                std::vector<std::string> otherAccepted;
                Judgement judgement = checkUnit(
                    other, originalOf(other, key, targets, originals, timing), i, planned,
                    rewritten, symbolsBefore, plan, signature, otherAccepted, renamed, timing);
                if (judgement.verdict == Verdict::Indeterminate &&
                    judgement.reason == kDoesNotCompileAsItIs)
                {
                    judgement.reason = unit.filename + " does not compile with Clang as it is";
                }
                judged.push_back(label(key, &other));
                judgements.push_back(std::move(judgement));
                for (const std::string& line : otherAccepted)
                {
                    if (std::find(accepted.begin(), accepted.end(), line) == accepted.end())
                        accepted.push_back(line);
                }
            }
            const Judgement verdict = const_suggestion::aggregate(judgements);
            if (verdict.verdict == Verdict::Refuted)
            {
                trace(issue, "removed, " + verdict.reason);
                traceUnits(issue, judged, judgements);
                continue;
            }
            if (verdict.verdict == Verdict::Indeterminate)
            {
                issue.checkLines.push_back(kNotChecked + verdict.reason);
                trace(issue, "not checked, " + verdict.reason);
                traceUnits(issue, judged, judgements);
                kept.push_back(std::move(issue));
                continue;
            }
            trace(issue, "checked in " + std::to_string(judgements.size()) + " file(s)");
            traceUnits(issue, judged, judgements);
            const std::string checked =
                std::string(config.compilationDatabase ? kCheckedDatabase : kCheckedAnalyzed) +
                kCheckedResult;
            issue.checkLines.push_back(
                accepted.empty() ? checked : checked + ", except for the differences below");
            issue.checkLines.insert(issue.checkLines.end(), accepted.begin(), accepted.end());
            const llvm::Function* function = mod.getFunction(issue.funcName);
            if (function && function->hasExternalLinkage() && !renamed.empty() &&
                renamed != issue.funcName)
            {
                issue.checkLines.push_back("ABI: the suggested type changes the symbol (" +
                                           issue.funcName + " -> " + renamed +
                                           "): code compiled separately must use the updated "
                                           "declaration and be recompiled");
            }
            kept.push_back(std::move(issue));
        }
        issues = std::move(kept);
    }
} // namespace ctrace::stack::analysis
