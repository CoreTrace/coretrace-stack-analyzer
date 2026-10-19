// SPDX-License-Identifier: Apache-2.0
// Compiles one unit for the Clang check of const suggestions (#217) and inspects it: its
// diagnostics, its generated code, and what it shows of the functions the suggestions name.
#include "ConstSuggestionInternal.hpp"
#include "analysis/InputPipeline.hpp"
#include "analyzer/HotspotProfiler.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/RecursiveASTVisitor.h>
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
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/xxhash.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::Compilation;
        using const_suggestion::FunctionFacts;
        using const_suggestion::realPath;
        using const_suggestion::Target;
        using const_suggestion::variantOf;
        using const_suggestion::withoutConst;
        using const_suggestion::writtenType;

        // Warnings that the C and C++ standards make constraint violations, or that a rewrite
        // turns on silently: the rewritten code must not add any.
        constexpr const char* kCheckedWarnings[] = {
            "incompatible-pointer-types-discards-qualifiers", "incompatible-pointer-types",
            "incompatible-function-pointer-types", "int-conversion", "overloaded-virtual"};

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
                    facts.plan = const_suggestion::plan(*context_, out_.macros, *function,
                                                        target.sourceIndex);
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
                if (std::any_of(targets_.begin(), targets_.end(),
                                [](const Target& target) { return target.plan; }))
                {
                    ci.getPreprocessor().addPPCallbacks(const_suggestion::recordMacroDefinitions(
                        ci.getSourceManager(), out_.macros));
                }
                return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
            }

          private:
            std::vector<Target>& targets_;
            Compilation& out_;
        };

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
    } // namespace

    Compilation const_suggestion::compile(const UnitCommand& command,
                                          const std::map<std::string, std::string>& rewritten,
                                          std::vector<Target>& targets, std::string_view step,
                                          bool timing)
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
                    out.failure = "the compile command is not understood: " + out.firstDiagnostic;
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

    // The files a unit reads and its identifiers, from its preprocessing. Empty when the
    // unit cannot be preprocessed without error: what it reads is then unknown.
    UnitScan const_suggestion::scan(const UnitCommand& command, bool timing)
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
                if (!instance.hasSourceManager() || instance.getDiagnostics().hasErrorOccurred())
                    return;
                const clang::SourceManager& sm = instance.getSourceManager();
                for (auto it = sm.fileinfo_begin(); it != sm.fileinfo_end(); ++it)
                    out.files.insert(realPath(it->first.getName()));
            },
            error);
        return out;
    }
} // namespace ctrace::stack::analysis
