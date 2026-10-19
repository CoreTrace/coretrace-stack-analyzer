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
#include <optional>
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
        using const_suggestion::DeclarationSite;
        using const_suggestion::Edit;
        using const_suggestion::EntityIdentity;
        using const_suggestion::linkageOf;
        using const_suggestion::realPath;
        using const_suggestion::SourceLinkage;
        using const_suggestion::SourcePosition;
        using const_suggestion::SuggestionPlan;
        using const_suggestion::Target;
        using const_suggestion::UnitFacts;
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
            explicit DiagnosticCollector(UnitFacts& out) : out_(out) {}

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
            UnitFacts& out_;
        };

        // An offset of the rewritten text, in the original one. An offset inside inserted text
        // maps to the edit's position.
        std::size_t originalOffset(const std::vector<Edit>& sortedEdits, std::size_t offset)
        {
            std::size_t inserted = 0; // text the edits before `offset` add
            std::size_t removed = 0;  // and remove
            for (const Edit& edit : sortedEdits)
            {
                const std::size_t at = edit.offset + inserted - removed;
                if (offset < at)
                    break;
                if (offset < at + edit.text.size())
                    return edit.offset;
                inserted += edit.text.size();
                removed += edit.length;
            }
            return offset - inserted + removed;
        }

        // The symbol CodeGen gives each variant of a function: a constructor's complete and base
        // ones, a destructor's deleting, complete and base ones, otherwise the function's. It
        // numbers unnamed types, such as a lambda ($_0, $_1), in the order it mangles them, as the
        // IR names them.
        std::vector<std::string> variantSymbols(clang::CodeGenerator& generator,
                                                const clang::FunctionDecl& function)
        {
            if (function.isDependentContext())
                return {};
            const auto symbol = [&](clang::GlobalDecl variant)
            { return generator.GetMangledName(variant).str(); };
            if (const auto* constructor = llvm::dyn_cast<clang::CXXConstructorDecl>(&function))
            {
                return {symbol({constructor, clang::Ctor_Complete}),
                        symbol({constructor, clang::Ctor_Base})};
            }
            if (const auto* destructor = llvm::dyn_cast<clang::CXXDestructorDecl>(&function))
            {
                return {symbol({destructor, clang::Dtor_Deleting}),
                        symbol({destructor, clang::Dtor_Complete}),
                        symbol({destructor, clang::Dtor_Base})};
            }
            return {symbol(clang::GlobalDecl(&function))};
        }

        // Each target's function, found by its declarations (§3, §5.5), the explicit casts of its
        // address, and what each method of its name overrides.
        class Inspector : public clang::ASTConsumer, public clang::RecursiveASTVisitor<Inspector>
        {
          public:
            Inspector(clang::CodeGenAction& action, const std::vector<Target>& targets,
                      const SuggestionPlan* rewrittenBy, UnitFacts& out)
                : action_(action), targets_(targets), rewrittenBy_(rewrittenBy), out_(out)
            {
            }

            bool shouldVisitTemplateInstantiations() const
            {
                return true;
            }

            void HandleTranslationUnit(clang::ASTContext& context) override
            {
                context_ = &context;
                generator_ = action_.getCodeGenerator();
                sites_.emplace(context.getSourceManager(), rewrittenBy_);
                out_.entities.resize(targets_.size());
                located_.assign(targets_.size(), nullptr);
                atSites_.assign(targets_.size(), {});
                linked_.assign(targets_.size(), {});
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    if (!targets_[i].plan)
                        planAt(i);
                }
                // The files of the planned declarations: no other file has their sites.
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    for (const DeclarationSite& site : identity(i).redeclarations)
                        declarationFiles_.insert(site.expansion.file);
                }
                const analyzer::ScopedHotspot hotspot(out_.timing, "const_suggestion.inspect");
                TraverseDecl(context.getTranslationUnitDecl());
                for (std::size_t i = 0; i < targets_.size(); ++i)
                    resolve(i);
            }

            bool VisitFunctionDecl(clang::FunctionDecl* function)
            {
                const clang::SourceManager& sm = context_->getSourceManager();
                const clang::SourceLocation location = function->getLocation();
                std::optional<DeclarationSite> site;
                if (location.isValid() && declarationFiles_.count(sites_->fileOf(
                                              sm.getFileID(sm.getExpansionLoc(location)))))
                {
                    site = (*sites_)(location);
                }
                const std::string& name = nameOf(function->getDeclName());
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    const EntityIdentity& entity = identity(i);
                    if (targets_[i].plan)
                    {
                        if (site && std::binary_search(entity.redeclarations.begin(),
                                                       entity.redeclarations.end(), *site))
                        {
                            atSites_[i].push_back(function);
                        }
                        else if (entity.linkage == SourceLinkage::External && name == entity.name &&
                                 linkageOf(*function) == SourceLinkage::External)
                        {
                            linked_[i].push_back(function);
                        }
                    }
                    const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
                    if (method && name == entity.name)
                        recordOverrides(out_.entities[i], *method);
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
                if (function)
                    casts_.push_back(function->getCanonicalDecl());
                return true;
            }

          private:
            void recordOverrides(UnitFacts::Entity& facts, const clang::CXXMethodDecl& method)
            {
                std::vector<std::string> overridden;
                for (const clang::CXXMethodDecl* base : method.overridden_methods())
                    overridden.push_back(base->getQualifiedNameAsString());
                std::sort(overridden.begin(), overridden.end());
                facts.overrides.insert(method.getQualifiedNameAsString() + " -> " +
                                       llvm::join(overridden, ", "));
            }

            const std::string& nameOf(clang::DeclarationName name)
            {
                auto [it, added] = names_.try_emplace(name.getAsOpaquePtr());
                if (added)
                    it->second = name.getAsString();
                return it->second;
            }

            const EntityIdentity& identity(std::size_t i) const
            {
                return targets_[i].plan ? targets_[i].plan->entity : out_.entities[i].plan.entity;
            }

            // In the defining unit's initial compilation, the function is the one CodeGen emits
            // under the suggestion's symbol.
            void planAt(std::size_t i)
            {
                if (!generator_)
                    return;
                const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(
                    generator_->GetDeclForMangledName(targets_[i].symbol));
                located_[i] = function;
                if (!function)
                    return;
                UnitFacts::Entity& facts = out_.entities[i];
                facts.plan = const_suggestion::plan(*context_, out_.macros, *function,
                                                    targets_[i].sourceIndex);
                facts.plan.entity.symbols = variantSymbols(*generator_, *function);
            }

            // The target's function in this compilation, and after the rewrite T v1's checks of
            // its declarations.
            void resolve(std::size_t i)
            {
                if (!generator_)
                    return;
                const Target& target = targets_[i];
                const EntityIdentity& entity = identity(i);
                UnitFacts::Entity& facts = out_.entities[i];
                std::vector<const clang::FunctionDecl*> found; // canonical declarations
                const auto add = [&](const clang::FunctionDecl* function)
                {
                    const clang::FunctionDecl* canonical = function->getCanonicalDecl();
                    if (std::find(found.begin(), found.end(), canonical) == found.end())
                        found.push_back(canonical);
                };
                const auto named = [&](const clang::FunctionDecl* function)
                {
                    const std::vector<std::string> symbols = variantSymbols(*generator_, *function);
                    return std::find_first_of(symbols.begin(), symbols.end(),
                                              entity.symbols.begin(),
                                              entity.symbols.end()) != symbols.end();
                };
                if (!target.plan)
                {
                    if (located_[i])
                        add(located_[i]);
                }
                else if (!atSites_[i].empty())
                {
                    // Its declarations have its sites. A specialization has its pattern's, as the
                    // other specializations of its template: it is the one with its symbol.
                    for (const clang::FunctionDecl* function : atSites_[i])
                    {
                        if (!function->isDependentContext() &&
                            (!entity.templateOrigin || named(function)))
                        {
                            add(function);
                        }
                    }
                }
                else
                {
                    // A unit that shares none of its sites declares it only through the linker.
                    for (const clang::FunctionDecl* function : linked_[i])
                    {
                        if (!function->isDependentContext() && named(function))
                            add(function);
                    }
                }
                facts.found = !found.empty();
                if (!facts.found)
                    return;
                facts.symbols = variantSymbols(*generator_, *found.front());
                facts.printedSignature = clang::PredefinedExpr::ComputeName(
                    clang::PredefinedIdentKind::PrettyFunction, found.front());
                if (!target.plan || target.plan != rewrittenBy_)
                    return;
                const clang::SourceManager& sm = context_->getSourceManager();
                for (const clang::FunctionDecl* function : found)
                {
                    for (const clang::FunctionDecl* declaration : function->redecls())
                    {
                        if (!facts.problem.empty() || declaration->isDependentContext() ||
                            target.plan->sourceIndex >= declaration->getNumParams())
                        {
                            continue;
                        }
                        const std::string type = writtenType(
                            *context_,
                            declaration->getParamDecl(target.plan->sourceIndex)->getType());
                        if (type == target.plan->expectedType)
                            continue;
                        const clang::SourceLocation at =
                            sm.getExpansionLoc(declaration->getLocation());
                        const std::string& file = sites_->fileOf(sm.getFileID(at));
                        facts.problem = target.plan->edits.count(file)
                                            ? "rewritten:" + type
                                            : "declared in " +
                                                  llvm::sys::path::filename(file).str() + ":" +
                                                  std::to_string(sm.getExpansionLineNumber(at)) +
                                                  " with type " + type;
                    }
                }
                if (facts.problem.empty() &&
                    std::find_first_of(casts_.begin(), casts_.end(), found.begin(), found.end()) !=
                        casts_.end())
                {
                    facts.problem = "its address is converted by an explicit cast";
                }
            }

            std::uint8_t reservedPadding_[7] = {};
            clang::CodeGenAction& action_;
            const std::vector<Target>& targets_;
            const SuggestionPlan* rewrittenBy_;
            UnitFacts& out_;
            clang::ASTContext* context_ = nullptr;
            clang::CodeGenerator* generator_ = nullptr;
            std::optional<const_suggestion::SiteReader> sites_;
            std::set<std::string> declarationFiles_;
            std::map<void*, std::string> names_;
            std::vector<const clang::FunctionDecl*> located_;
            std::vector<std::vector<const clang::FunctionDecl*>> atSites_; // with one of its sites
            std::vector<std::vector<const clang::FunctionDecl*>> linked_;  // and the linker's
            std::vector<const clang::FunctionDecl*> casts_;                // explicitly cast
        };

        class InspectAction : public clang::EmitLLVMOnlyAction
        {
          public:
            InspectAction(llvm::LLVMContext* context, const std::vector<Target>& targets,
                          const SuggestionPlan* rewrittenBy, UnitFacts& out)
                : clang::EmitLLVMOnlyAction(context), targets_(targets), rewrittenBy_(rewrittenBy),
                  out_(out)
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
                consumers.push_back(
                    std::make_unique<Inspector>(*this, targets_, rewrittenBy_, out_));
                if (std::any_of(targets_.begin(), targets_.end(),
                                [](const Target& target) { return !target.plan; }))
                {
                    ci.getPreprocessor().addPPCallbacks(const_suggestion::recordMacroDefinitions(
                        ci.getSourceManager(), out_.macros));
                }
                return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
            }

          private:
            const std::vector<Target>& targets_;
            const SuggestionPlan* rewrittenBy_;
            UnitFacts& out_;
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

    UnitFacts const_suggestion::inspect(const UnitCommand& command,
                                        const SuggestionPlan* rewrittenBy,
                                        const std::map<std::string, std::string>& rewritten,
                                        const std::vector<Target>& targets, std::string_view step,
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
        UnitFacts out;
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
                InspectAction action(out.context.get(), targets, rewrittenBy, out);
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

    const_suggestion::SiteReader::SiteReader(const clang::SourceManager& sm,
                                             const SuggestionPlan* rewrittenBy)
        : sm_(sm)
    {
        if (!rewrittenBy)
            return;
        edits_ = rewrittenBy->edits;
        for (auto& [path, edits] : edits_)
        {
            std::sort(edits.begin(), edits.end(),
                      [](const Edit& a, const Edit& b) { return a.offset < b.offset; });
        }
    }

    DeclarationSite const_suggestion::SiteReader::operator()(clang::SourceLocation location)
    {
        if (location.isInvalid())
            return {};
        return {position(sm_.getExpansionLoc(location)), position(sm_.getSpellingLoc(location))};
    }

    const std::string& const_suggestion::SiteReader::fileOf(clang::FileID file)
    {
        const auto found = files_.find(file);
        if (found != files_.end())
            return found->second;
        const clang::OptionalFileEntryRef entry = sm_.getFileEntryRefForID(file);
        std::string path = entry ? realPath(entry->getName())
                                 : sm_.getBufferName(sm_.getLocForStartOfFile(file)).str();
        return files_.emplace(file, std::move(path)).first->second;
    }

    SourcePosition const_suggestion::SiteReader::position(clang::SourceLocation location)
    {
        const auto [file, offset] = sm_.getDecomposedLoc(location);
        SourcePosition out{fileOf(file), offset};
        const auto edits = edits_.find(out.file);
        if (edits != edits_.end())
            out.offset = originalOffset(edits->second, out.offset);
        return out;
    }
} // namespace ctrace::stack::analysis
