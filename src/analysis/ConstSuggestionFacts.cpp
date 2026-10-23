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
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/GlobalDecl.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/TemplateBase.h>
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
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/xxhash.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::candidateOf;
        using const_suggestion::DeclarationSite;
        using const_suggestion::Edit;
        using const_suggestion::EntityIdentity;
        using const_suggestion::Fact;
        using const_suggestion::FactKind;
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
                if (level == clang::DiagnosticsEngine::Fatal && out_.fatal.empty())
                    out_.fatal = out_.reported.back().text;
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

        // The variant each of variantSymbols names, in the same order.
        std::vector<std::string> variantKeys(const clang::FunctionDecl& function)
        {
            if (function.isDependentContext())
                return {};
            if (llvm::isa<clang::CXXConstructorDecl>(function))
                return {"C1", "C2"};
            if (llvm::isa<clang::CXXDestructorDecl>(function))
                return {"D0", "D1", "D2"};
            return {""};
        }

        // A specialization's template arguments, its class's then its own, each type written
        // canonically (ruling 7); "" for a function that is no specialization.
        std::string argumentsOf(const clang::ASTContext& context,
                                const clang::FunctionDecl& function)
        {
            clang::PrintingPolicy policy(context.getLangOpts());
            policy.AnonymousTagLocations = false;
            std::string out;
            const auto add = [&](llvm::ArrayRef<clang::TemplateArgument> arguments)
            {
                out += "<";
                for (const clang::TemplateArgument& argument : arguments)
                {
                    if (argument.getKind() == clang::TemplateArgument::Type)
                    {
                        out += writtenType(context, argument.getAsType());
                    }
                    else
                    {
                        llvm::raw_string_ostream text(out);
                        argument.print(policy, text, /*IncludeType=*/true);
                    }
                    out += ",";
                }
                out += ">";
            };
            if (const auto* record = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                    function.getDeclContext()))
            {
                add(record->getTemplateArgs().asArray());
            }
            if (const clang::TemplateArgumentList* arguments =
                    function.getTemplateSpecializationArgs())
            {
                add(arguments->asArray());
            }
            return out;
        }

        // Whether a declaration comes from a template instantiation: a specialization the
        // compilation instantiates, or a declaration inside one.
        bool instantiated(const clang::Decl* declaration)
        {
            while (declaration)
            {
                const auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration);
                const auto* variable = llvm::dyn_cast<clang::VarDecl>(declaration);
                const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(declaration);
                if ((function && function->isTemplateInstantiation()) ||
                    (variable &&
                     clang::isTemplateInstantiation(variable->getTemplateSpecializationKind())) ||
                    (record &&
                     clang::isTemplateInstantiation(record->getTemplateSpecializationKind())))
                {
                    return true;
                }
                const clang::DeclContext* context = declaration->getDeclContext();
                declaration = context ? clang::Decl::castFromDeclContext(context) : nullptr;
            }
            return false;
        }

        // A site, as an R or O value holds it.
        std::string textOf(const DeclarationSite& site)
        {
            return site.expansion.file + ":" + std::to_string(site.expansion.offset) + "|" +
                   site.spelling.file + ":" + std::to_string(site.spelling.offset);
        }

        // Each target's function, found by its declarations (§3, §5.5), the explicit casts of its
        // address, and the facts R, O and K (§5.4).
        // §5.6 (D19): a type as the source writes it. A deduced part (auto, decltype(auto),
        // decltype, typeof, a deduced class template's arguments) is written as its keyword, the
        // rest canonically, so that what deduction gives it, a consequence of the plan, is left
        // to steps 4 to 7. `deduced` tells whether the type has such a part.
        std::string writtenForm(const clang::ASTContext& context, clang::QualType type,
                                bool& deduced)
        {
            const clang::PrintingPolicy policy(context.getLangOpts());
            const std::string qualifiers = type.getLocalQualifiers().getAsString(policy);
            const auto qualified = [&](const std::string& text)
            { return qualifiers.empty() ? text : qualifiers + " " + text; };
            const auto part = [&](clang::QualType inner)
            { return writtenForm(context, inner, deduced); };
            const clang::Type* node = type.getTypePtr();
            if (const auto* deducedTemplate =
                    llvm::dyn_cast<clang::DeducedTemplateSpecializationType>(node))
            {
                deduced = true;
                std::string name;
                llvm::raw_string_ostream out(name);
                deducedTemplate->getTemplateName().print(out, policy);
                return qualified(name);
            }
            if (const auto* automatic = llvm::dyn_cast<clang::AutoType>(node))
            {
                deduced = true;
                return qualified(automatic->isDecltypeAuto() ? "decltype(auto)" : "auto");
            }
            if (llvm::isa<clang::DecltypeType, clang::TypeOfExprType, clang::TypeOfType>(node))
            {
                deduced = true;
                return qualified("decltype");
            }
            if (const auto* pointer = llvm::dyn_cast<clang::PointerType>(node))
                return qualified(part(pointer->getPointeeType()) + " *");
            if (const auto* reference = llvm::dyn_cast<clang::ReferenceType>(node))
            {
                return qualified(part(reference->getPointeeTypeAsWritten()) +
                                 (reference->isLValueReferenceType() ? " &" : " &&"));
            }
            if (const auto* member = llvm::dyn_cast<clang::MemberPointerType>(node))
            {
                return qualified(part(member->getPointeeType()) + " " +
                                 writtenType(context, clang::QualType(member->getClass(), 0)) +
                                 "::*");
            }
            if (const auto* array = llvm::dyn_cast<clang::ArrayType>(node))
            {
                const auto* constant = llvm::dyn_cast<clang::ConstantArrayType>(array);
                return qualified(part(array->getElementType()) + " [" +
                                 (constant ? llvm::toString(constant->getSize(), 10, false) : "") +
                                 "]");
            }
            if (const auto* function = llvm::dyn_cast<clang::FunctionProtoType>(node))
            {
                std::string out = part(function->getReturnType()) + " (";
                for (unsigned i = 0; i < function->getNumParams(); ++i)
                    out += (i ? ", " : "") + part(function->getParamType(i));
                out += function->isVariadic() ? ", ...)" : ")";
                const std::string method = function->getMethodQuals().getAsString(policy);
                return qualified(method.empty() ? out : out + " " + method);
            }
            if (const auto* specialization =
                    llvm::dyn_cast<clang::TemplateSpecializationType>(node);
                specialization && !specialization->isTypeAlias())
            {
                std::string out;
                llvm::raw_string_ostream text(out);
                specialization->getTemplateName().print(text, policy);
                text << "<";
                bool first = true;
                for (const clang::TemplateArgument& argument : specialization->template_arguments())
                {
                    text << (first ? "" : ", ");
                    first = false;
                    if (argument.getKind() == clang::TemplateArgument::Type)
                        text << part(argument.getAsType());
                    else
                        argument.print(policy, text, /*IncludeType=*/true);
                }
                text << ">";
                return qualified(out);
            }
            if (llvm::isa<clang::ElaboratedType, clang::ParenType, clang::AttributedType,
                          clang::MacroQualifiedType>(node))
            {
                return qualified(part(node->getLocallyUnqualifiedSingleStepDesugaredType()));
            }
            return qualified(writtenType(context, clang::QualType(node, 0)));
        }

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

            // The instantiations around a fact, by their symbols.
            bool TraverseDecl(clang::Decl* declaration)
            {
                const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(declaration);
                const bool instantiation = collectFacts_ && function &&
                                           function->isTemplateInstantiation() &&
                                           !symbolsOf(*function).empty();
                if (instantiation)
                {
                    contexts_.push_back(symbolsOf(*function).front());
                    out_.instantiations.insert(contexts_.back());
                }
                const bool traversed =
                    clang::RecursiveASTVisitor<Inspector>::TraverseDecl(declaration);
                if (instantiation)
                    contexts_.pop_back();
                return traversed;
            }

            void HandleTranslationUnit(clang::ASTContext& context) override
            {
                context_ = &context;
                generator_ = action_.getCodeGenerator();
                mangle_.reset(context.createMangleContext());
                // After an error, the rewrite is refuted by it (§5.8, step 4): no fact is needed.
                collectFacts_ = generator_ && !context.getDiagnostics().hasErrorOccurred();
                sites_.emplace(context.getSourceManager(), rewrittenBy_);
                out_.file = sites_->fileOf(context.getSourceManager().getMainFileID());
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
                // The files that a plan rewrites: after the rewrite, those of its plan.
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    const SuggestionPlan& plan =
                        targets_[i].plan ? *targets_[i].plan : out_.entities[i].plan;
                    if (rewrittenBy_ && &plan != rewrittenBy_)
                        continue;
                    for (const auto& [path, edits] : plan.edits)
                        rewrittenFiles_.insert(path);
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
                    if (collectFacts_ && method && method->isFirstDecl() && name == entity.name)
                        out_.entities[i].overrides.push_back(overridesOf(*method));
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
                if (function && cast->getBeginLoc().isValid())
                    casts_.push_back({function->getCanonicalDecl(), cast->getBeginLoc()});
                return true;
            }

            // Nested symbols (§5.7), which the rewrite renames with the function around them: a
            // lambda's call operator, a local class's methods and type information, a static
            // local variable and its guard.
            bool VisitLambdaExpr(clang::LambdaExpr* lambda)
            {
                const clang::CXXMethodDecl* call = lambda->getCallOperator();
                if (!collectFacts_ || !call || call->isDependentContext())
                    return true;
                const std::vector<std::string>& symbols = symbolsOf(*call);
                if (!symbols.empty())
                {
                    nested(lambda->getLambdaClass()->getDeclContext(), lambda->getBeginLoc(),
                           {{symbols.front(), "lambda"}});
                }
                return true;
            }

            bool VisitCXXRecordDecl(clang::CXXRecordDecl* record)
            {
                if (!collectFacts_ || record->isDependentType() ||
                    !record->isThisDeclarationADefinition())
                {
                    return true;
                }
                inheritingIn(*record);
                if (!record->isLocalClass() || record->isLambda())
                    return true;
                const clang::QualType type = context_->getRecordType(record);
                std::string information;
                std::string name;
                {
                    llvm::raw_string_ostream out(information);
                    mangle_->mangleCXXRTTI(type, out);
                }
                {
                    llvm::raw_string_ostream out(name);
                    mangle_->mangleCXXRTTIName(type, out);
                }
                std::vector<std::pair<std::string, std::string>> symbols = {
                    {information, "type information"}, {name, "type name"}};
                auto* itanium = llvm::dyn_cast<clang::ItaniumMangleContext>(mangle_.get());
                if (itanium && record->isDynamicClass())
                {
                    std::string table;
                    llvm::raw_string_ostream out(table);
                    itanium->mangleCXXVTable(record, out);
                    symbols.push_back({table, "virtual table"});
                }
                nested(record->getDeclContext(), record->getLocation(), symbols);
                for (const clang::CXXMethodDecl* method : record->methods())
                {
                    const std::vector<std::string>& variants = symbolsOf(*method);
                    const std::vector<std::string> keys = variantKeys(*method);
                    std::vector<std::pair<std::string, std::string>> methods;
                    for (std::size_t k = 0; k < variants.size() && k < keys.size(); ++k)
                        methods.push_back({variants[k], "method " + keys[k]});
                    nested(record->getDeclContext(), method->getLocation(), methods);
                }
                return true;
            }

            // §5.6 (D19), after the rewrite: the planned parameters' types are the plan's. A type
            // that an edit writes anywhere else, in a declaration, an expression or a planned
            // parameter's default argument, is outside it.
            bool TraverseParmVarDecl(clang::ParmVarDecl* parameter)
            {
                const clang::TypeSourceInfo* outer = plannedType_;
                if (rewrittenBy_ && parameter->getTypeSourceInfo() &&
                    std::binary_search(rewrittenBy_->parameters.begin(),
                                       rewrittenBy_->parameters.end(),
                                       (*sites_)(parameter->getLocation())))
                {
                    plannedType_ = parameter->getTypeSourceInfo();
                }
                const bool traversed =
                    clang::RecursiveASTVisitor<Inspector>::TraverseParmVarDecl(parameter);
                plannedType_ = outer;
                return traversed;
            }

            bool TraverseTypeLoc(clang::TypeLoc loc)
            {
                const bool planned = plannedType_ && loc == plannedType_->getTypeLoc();
                inPlannedType_ += planned;
                const bool traversed = clang::RecursiveASTVisitor<Inspector>::TraverseTypeLoc(loc);
                inPlannedType_ -= planned;
                return traversed;
            }

            bool VisitTypeLoc(clang::TypeLoc loc)
            {
                if (!rewrittenBy_ || inPlannedType_ || out_.writtenOutside)
                    return true;
                const clang::SourceLocation at = loc.getBeginLoc();
                if (at.isInvalid())
                    return true;
                const clang::SourceManager& sm = context_->getSourceManager();
                const clang::FileID file = sm.getFileID(sm.getSpellingLoc(at));
                const auto known = rewrittenSpelling_.find(file.getHashValue());
                const bool rewritten = known != rewrittenSpelling_.end()
                                           ? known->second
                                           : (rewrittenSpelling_[file.getHashValue()] =
                                                  rewrittenFiles_.count(sites_->fileOf(file)) != 0);
                if (!rewritten)
                    return true;
                const DeclarationSite site = (*sites_)(at);
                if (sites_->edited(site.spelling))
                    out_.writtenOutside = UnitFacts::Outside{site, lineOf(at)};
                return true;
            }

            bool VisitVarDecl(clang::VarDecl* variable)
            {
                if (!collectFacts_ || !variable->isStaticLocal() ||
                    variable->getDeclContext()->isDependentContext())
                {
                    return true;
                }
                std::string guard;
                {
                    llvm::raw_string_ostream out(guard);
                    mangle_->mangleStaticGuardVariable(variable, out);
                }
                nested(variable->getDeclContext(), variable->getLocation(),
                       {{generator_->GetMangledName(clang::GlobalDecl(variable)).str(), "static"},
                        {guard, "guard"}});
                return true;
            }

            // §5.6: the type of each declaration that a rewritten file writes, by its site.
            bool VisitDeclaratorDecl(const clang::DeclaratorDecl* declaration)
            {
                written(*declaration, declaration->getType());
                return true;
            }

            bool VisitTypedefNameDecl(const clang::TypedefNameDecl* declaration)
            {
                written(*declaration, declaration->getUnderlyingType());
                return true;
            }

            // R: what a call or a reference targets, unevaluated operands and instantiations
            // included.
            bool VisitDeclRefExpr(clang::DeclRefExpr* expression)
            {
                reference(expression->getDecl(), expression->getLocation());
                return true;
            }

            bool VisitMemberExpr(const clang::MemberExpr* expression)
            {
                reference(expression->getMemberDecl(), expression->getMemberLoc());
                return true;
            }

            bool VisitCXXConstructExpr(const clang::CXXConstructExpr* expression)
            {
                reference(expression->getConstructor(), expression->getLocation());
                return true;
            }

            bool VisitCXXNewExpr(const clang::CXXNewExpr* expression)
            {
                reference(expression->getOperatorNew(), expression->getBeginLoc());
                return true;
            }

            bool VisitCXXDeleteExpr(const clang::CXXDeleteExpr* expression)
            {
                reference(expression->getOperatorDelete(), expression->getBeginLoc());
                return true;
            }

            // K: each if constexpr condition, and the association each _Generic selects.
            bool VisitIfStmt(clang::IfStmt* statement)
            {
                const clang::Expr* condition = statement->getCond();
                bool value = false;
                if (collectFacts_ && statement->isConstexpr() && condition &&
                    !condition->isValueDependent() &&
                    condition->EvaluateAsBooleanCondition(value, *context_))
                {
                    constant(statement->getIfLoc(), value ? "true" : "false", "");
                }
                return true;
            }

            bool VisitGenericSelectionExpr(const clang::GenericSelectionExpr* selection)
            {
                if (collectFacts_ && !selection->isResultDependent())
                {
                    constant(selection->getGenericLoc(),
                             std::to_string(selection->getResultIndex()), "_Generic");
                }
                return true;
            }

          private:
            // A symbol, cached by declaration.
            const std::vector<std::string>& symbolsOf(const clang::FunctionDecl& function)
            {
                const clang::FunctionDecl* canonical = function.getCanonicalDecl();
                auto [it, added] = symbols_.try_emplace(canonical);
                if (added)
                    it->second = variantSymbols(*generator_, *canonical);
                return it->second;
            }

            // A function, as a reason names it.
            const std::string& shownOf(const clang::FunctionDecl& function)
            {
                const clang::FunctionDecl* canonical = function.getCanonicalDecl();
                auto [it, added] = shown_.try_emplace(canonical);
                if (added)
                {
                    const std::vector<std::string>& symbols = symbolsOf(*canonical);
                    it->second = symbols.empty() ? canonical->getQualifiedNameAsString()
                                                 : llvm::demangle(symbols.front());
                }
                return it->second;
            }

            // An R value (ruling 4): the target's declaration sites; a specialization or an
            // implicit function, which has none of its own, by its symbol.
            const std::vector<std::string>& valueOf(const clang::FunctionDecl& target)
            {
                const clang::FunctionDecl* canonical = target.getCanonicalDecl();
                auto [it, added] = values_.try_emplace(canonical);
                std::vector<std::string>& value = it->second;
                if (!added)
                    return value;
                if (canonical->isImplicit() || canonical->isTemplateInstantiation())
                {
                    const std::vector<std::string>& symbols = symbolsOf(*canonical);
                    if (symbols.empty())
                        return value;
                    value.push_back("symbol:" + symbols.front());
                    // A specialization also names its template: two specializations of one
                    // template may correspond (§5.7).
                    if (const clang::FunctionDecl* pattern =
                            canonical->getTemplateInstantiationPattern(/*ForDefinition=*/false))
                    {
                        value.push_back("template:" +
                                        textOf((*sites_)(pattern->getFirstDecl()->getLocation())));
                    }
                    return value;
                }
                for (const clang::FunctionDecl* declaration : canonical->redecls())
                    value.push_back("site:" + textOf((*sites_)(declaration->getLocation())));
                std::sort(value.begin(), value.end());
                value.erase(std::unique(value.begin(), value.end()), value.end());
                return value;
            }

            // Neither implicit nor instantiated, valid or not: a declaration that the rewrite
            // makes invalid outside the plan is still one that it changes.
            void written(const clang::NamedDecl& declaration, clang::QualType type)
            {
                const clang::SourceLocation at = declaration.getLocation();
                if (rewrittenFiles_.empty() || at.isInvalid() || declaration.isImplicit() ||
                    instantiated(&declaration))
                {
                    return;
                }
                const clang::SourceManager& sm = context_->getSourceManager();
                if (!rewrittenFiles_.count(sites_->fileOf(sm.getFileID(sm.getExpansionLoc(at)))))
                    return;
                std::string name = declaration.getNameAsString();
                bool deduced = false;
                std::string form = writtenForm(*context_, type, deduced);
                out_.writtenTypes[(*sites_)(at)].push_back(UnitFacts::Written{
                    deduced ? std::move(form) : writtenType(*context_, type),
                    name.empty() ? "an unnamed declaration" : std::move(name), lineOf(at)});
            }

            unsigned lineOf(clang::SourceLocation at) const
            {
                return context_->getSourceManager().getExpansionLineNumber(at);
            }

            void reference(const clang::ValueDecl* declaration, clang::SourceLocation at)
            {
                const auto* target = llvm::dyn_cast_or_null<clang::FunctionDecl>(declaration);
                if (!collectFacts_ || !target || at.isInvalid())
                    return;
                const std::vector<std::string>& value = valueOf(*target);
                if (value.empty())
                    return;
                out_.facts.push_back({{contexts_, (*sites_)(at)},
                                      value,
                                      "",
                                      shownOf(*target),
                                      lineOf(at),
                                      FactKind::Reference});
            }

            void constant(clang::SourceLocation at, std::string value, std::string subject)
            {
                out_.facts.push_back({{contexts_, (*sites_)(at)},
                                      {std::move(value)},
                                      std::move(subject),
                                      "",
                                      lineOf(at),
                                      FactKind::Constant});
            }

            // O: the methods that a method of the entity's name overrides.
            Fact overridesOf(const clang::CXXMethodDecl& method)
            {
                Fact out{{contexts_, (*sites_)(method.getLocation())},
                         {},
                         shownOf(method),
                         "",
                         lineOf(method.getLocation()),
                         FactKind::Override};
                std::vector<std::string> shown;
                for (const clang::CXXMethodDecl* base : method.overridden_methods())
                {
                    out.value.push_back(llvm::join(valueOf(*base), ";"));
                    shown.push_back("'" + shownOf(*base) + "'");
                }
                std::sort(out.value.begin(), out.value.end());
                std::sort(shown.begin(), shown.end());
                out.shown = shown.empty() ? "nothing" : llvm::join(shown, ", ");
                return out;
            }

            const std::string& nameOf(clang::DeclarationName name)
            {
                auto [it, added] = names_.try_emplace(name.getAsOpaquePtr());
                if (added)
                    it->second = name.getAsString();
                return it->second;
            }

            // Whether a function has one of the sites of the entity of target i: it is one of
            // its declarations, or a specialization of its template.
            bool atSites(std::size_t i, const clang::FunctionDecl& function)
            {
                const clang::SourceLocation location = function.getLocation();
                if (location.isInvalid())
                    return false;
                const clang::SourceManager& sm = context_->getSourceManager();
                if (!declarationFiles_.count(
                        sites_->fileOf(sm.getFileID(sm.getExpansionLoc(location)))))
                {
                    return false;
                }
                const EntityIdentity& entity = identity(i);
                return std::binary_search(entity.redeclarations.begin(),
                                          entity.redeclarations.end(), (*sites_)(location));
            }

            // The symbol of the function around a nested declaration, when its outermost
            // function, through lambdas and local classes, is the entity of target i or a
            // specialization of its template; "" otherwise.
            std::string enclosingSymbol(std::size_t i, const clang::DeclContext* context)
            {
                const clang::FunctionDecl* immediate = nullptr;
                for (const clang::DeclContext* at = context; at; at = at->getParent())
                {
                    const auto* function = llvm::dyn_cast<clang::FunctionDecl>(at);
                    if (!function)
                        continue;
                    if (!immediate)
                        immediate = function;
                    const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(function);
                    if (method &&
                        (method->getParent()->isLambda() || method->getParent()->isLocalClass()))
                    {
                        continue;
                    }
                    if (function->isDependentContext() || !atSites(i, *function))
                        return "";
                    const std::vector<std::string>& symbols = symbolsOf(*immediate);
                    return symbols.empty() ? "" : symbols.front();
                }
                return "";
            }

            // Records nested symbols, each with its kind, for the targets whose entity they lie in.
            void nested(const clang::DeclContext* context, clang::SourceLocation at,
                        const std::vector<std::pair<std::string, std::string>>& symbols)
            {
                if (at.isInvalid())
                    return;
                for (std::size_t i = 0; i < targets_.size(); ++i)
                {
                    const std::string enclosing = enclosingSymbol(i, context);
                    if (enclosing.empty())
                        continue;
                    const std::string site = textOf((*sites_)(at));
                    for (const auto& [symbol, kind] : symbols)
                        out_.entities[i].nested.push_back({symbol, kind + "|" + site, enclosing});
                }
            }

            // The constructors that inherit the entity of a target, or a specialization of its
            // template (ruling 7): by derived class, inherited specialization and variant.
            void inheritingIn(const clang::CXXRecordDecl& record)
            {
                for (const clang::CXXConstructorDecl* constructor : record.ctors())
                {
                    if (!constructor->isInheritingConstructor())
                        continue;
                    const clang::CXXConstructorDecl* inherited =
                        constructor->getInheritedConstructor().getConstructor();
                    if (!inherited)
                        continue;
                    std::string derived;
                    {
                        llvm::raw_string_ostream out(derived);
                        mangle_->mangleCanonicalTypeName(context_->getRecordType(&record), out);
                    }
                    const std::string arguments = argumentsOf(*context_, *inherited);
                    const std::vector<std::string>& symbols = symbolsOf(*constructor);
                    const std::vector<std::string> keys = variantKeys(*constructor);
                    // The specialization it inherits too: only its implicit code calls it, so no
                    // reference pairs it.
                    const std::vector<std::string>& inheritedSymbols = symbolsOf(*inherited);
                    const std::vector<std::string> inheritedKeys = variantKeys(*inherited);
                    for (std::size_t i = 0; i < targets_.size(); ++i)
                    {
                        if (!atSites(i, *inherited))
                            continue;
                        for (std::size_t k = 0; k < symbols.size() && k < keys.size(); ++k)
                        {
                            out_.entities[i].inheriting.push_back(
                                {symbols[k], derived + "|" + arguments + "|" + keys[k], ""});
                        }
                        for (std::size_t k = 0;
                             k < inheritedSymbols.size() && k < inheritedKeys.size(); ++k)
                        {
                            out_.entities[i].inheriting.push_back(
                                {inheritedSymbols[k],
                                 "inherited|" + arguments + "|" + inheritedKeys[k], ""});
                        }
                    }
                }
            }

            const EntityIdentity& identity(std::size_t i) const
            {
                return targets_[i].plan ? targets_[i].plan->entity : out_.entities[i].plan.entity;
            }

            // §5.10, after the rewrite: each symbol the entity of target i takes that this
            // compilation defines. Another entity's definition is a collision, with its IR
            // linkage; the entity's own, such as the inline function of a rewritten header that
            // several files define, is none.
            void definitions(std::size_t i)
            {
                UnitFacts::Entity& facts = out_.entities[i];
                const llvm::Module* module = generator_->GetModule();
                std::vector<std::string> takes = targets_[i].takes;
                for (const const_suggestion::Named& variant : facts.variants)
                {
                    if (std::find(takes.begin(), takes.end(), variant.symbol) == takes.end())
                        takes.push_back(variant.symbol);
                }
                for (const std::string& symbol : takes)
                {
                    const llvm::GlobalValue* value =
                        module ? module->getNamedValue(symbol) : nullptr;
                    if (!value || value->isDeclaration())
                        continue;
                    if (!facts.linkage)
                        facts.linkage = value->getLinkage();
                    const auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(
                        generator_->GetDeclForMangledName(symbol));
                    const clang::FunctionDecl* definition =
                        function ? function->getDefinition() : nullptr;
                    if (definition && atSites(i, *definition))
                        continue;
                    const clang::SourceLocation at =
                        definition ? definition->getLocation() : clang::SourceLocation();
                    facts.collisions.push_back({symbol, (*sites_)(at), value->getLinkage(),
                                                at.isValid() ? lineOf(at) : 0});
                }
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
                facts.plan.entity.templateArguments = argumentsOf(*context_, *function);
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
                    const std::vector<std::string>& symbols = symbolsOf(*function);
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
                    // other specializations of its template: it is the one with its symbol, or
                    // once the rewrite renames it, the only one with its template arguments
                    // (ruling 7).
                    std::vector<const clang::FunctionDecl*> sameArguments;
                    for (const clang::FunctionDecl* function : atSites_[i])
                    {
                        if (function->isDependentContext())
                            continue;
                        if (!entity.templateOrigin || named(function))
                        {
                            add(function);
                        }
                        else if (!entity.templateArguments.empty() &&
                                 argumentsOf(*context_, *function) == entity.templateArguments &&
                                 std::find(sameArguments.begin(), sameArguments.end(),
                                           function->getCanonicalDecl()) == sameArguments.end())
                        {
                            sameArguments.push_back(function->getCanonicalDecl());
                        }
                    }
                    if (found.empty() && sameArguments.size() == 1)
                        add(sameArguments.front());
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
                if (facts.found)
                {
                    const std::vector<std::string>& symbols = symbolsOf(*found.front());
                    const std::vector<std::string> keys = variantKeys(*found.front());
                    for (std::size_t k = 0; k < symbols.size() && k < keys.size(); ++k)
                        facts.variants.push_back({symbols[k], keys[k], ""});
                    facts.printedSignature = clang::PredefinedExpr::ComputeName(
                        clang::PredefinedIdentKind::PrettyFunction, found.front());
                    for (const clang::FunctionDecl* function : found)
                    {
                        for (const clang::FunctionDecl* declaration : function->redecls())
                        {
                            const clang::SourceLocation at = declaration->getLocation();
                            if (at.isValid())
                                facts.declarations.emplace((*sites_)(at), lineOf(at));
                        }
                    }
                }
                // Before the rewrite, the type each declaration must get: the candidate of its
                // own parameter type, as this compilation's language writes it (D8, D18).
                const SuggestionPlan& plan = target.plan ? *target.plan : facts.plan;
                if (!rewrittenBy_ && !plan.indeterminate)
                {
                    for (const clang::FunctionDecl* function : found)
                    {
                        for (const clang::FunctionDecl* declaration : function->redecls())
                        {
                            if (declaration->isDependentContext() ||
                                plan.sourceIndex >= declaration->getNumParams())
                            {
                                continue;
                            }
                            const clang::QualType candidate = candidateOf(
                                *context_, declaration->getParamDecl(plan.sourceIndex)->getType());
                            if (!candidate.isNull())
                            {
                                facts.expected.emplace((*sites_)(declaration->getLocation()),
                                                       writtenType(*context_, candidate));
                            }
                        }
                    }
                }
                if (!target.plan || target.plan != rewrittenBy_)
                    return;
                definitions(i);
                if (!facts.found)
                    return;
                const clang::SourceManager& sm = context_->getSourceManager();
                for (const clang::FunctionDecl* function : found)
                {
                    for (const clang::FunctionDecl* declaration : function->redecls())
                    {
                        if (declaration->isDependentContext() ||
                            target.plan->sourceIndex >= declaration->getNumParams())
                        {
                            continue;
                        }
                        const std::string type = writtenType(
                            *context_,
                            declaration->getParamDecl(target.plan->sourceIndex)->getType());
                        // A declaration with no expected type was another entity's before the
                        // rewrite, which merges into it: the collisions judge it (ruling 3).
                        const auto expected =
                            target.expected.find((*sites_)(declaration->getLocation()));
                        if (expected == target.expected.end() || type == expected->second)
                            continue;
                        const clang::SourceLocation at =
                            sm.getExpansionLoc(declaration->getLocation());
                        const std::string& file = sites_->fileOf(sm.getFileID(at));
                        // A planned parameter must have the candidate of its own type, as its
                        // unit writes it (§5.6, step 3; D8, D18). A declaration in a file that the
                        // plan does not rewrite, through which the unit declares the entity for the
                        // linker only, keeps its type: the unit does not see the rewrite (D17).
                        if (target.plan->edits.count(file))
                            facts.withoutCandidate = true;
                        else
                            facts.stale = true;
                    }
                }
                // The explicit casts of its address, in source order (D15).
                std::vector<clang::SourceLocation> places;
                for (const auto& [function, at] : casts_)
                {
                    if (std::find(found.begin(), found.end(), function) != found.end())
                        places.push_back(sm.getExpansionLoc(at));
                }
                std::stable_sort(places.begin(), places.end(),
                                 [&](clang::SourceLocation a, clang::SourceLocation b)
                                 { return sm.isBeforeInTranslationUnit(a, b); });
                for (const clang::SourceLocation at : places)
                    facts.casts.push_back({(*sites_)(at), lineOf(at)});
            }

            bool collectFacts_ = false;
            std::uint8_t reservedPadding_[6] = {};
            clang::CodeGenAction& action_;
            const std::vector<Target>& targets_;
            const SuggestionPlan* rewrittenBy_;
            UnitFacts& out_;
            clang::ASTContext* context_ = nullptr;
            clang::CodeGenerator* generator_ = nullptr;
            std::optional<const_suggestion::SiteReader> sites_;
            std::unique_ptr<clang::MangleContext> mangle_; // for what CodeGen does not name
            std::set<std::string> declarationFiles_;
            std::set<std::string> rewrittenFiles_;
            std::map<void*, std::string> names_;
            std::vector<const clang::FunctionDecl*> located_;
            std::vector<std::vector<const clang::FunctionDecl*>> atSites_; // with one of its sites
            std::vector<std::vector<const clang::FunctionDecl*>> linked_;  // and the linker's
            // The functions whose address is cast explicitly, other than to void, and where.
            std::vector<std::pair<const clang::FunctionDecl*, clang::SourceLocation>> casts_;
            std::vector<std::string> contexts_; // the instantiations around, outermost first
            std::map<const clang::FunctionDecl*, std::vector<std::string>> symbols_;
            std::map<const clang::FunctionDecl*, std::vector<std::string>> values_;
            std::map<const clang::FunctionDecl*, std::string> shown_;
            // After the rewrite, the planned parameter's type being traversed (D19).
            const clang::TypeSourceInfo* plannedType_ = nullptr;
            std::size_t inPlannedType_ = 0;
            std::map<unsigned, bool> rewrittenSpelling_; // by FileID: is it a rewritten file
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
                // No error limit: "too many errors" is fatal, and would leave unchecked a rewrite
                // that its errors refute (ruling 12).
                diagnostics.ErrorLimit = 0;
                // D6: each tracked group as Clang reports it by default, whatever the command
                // says: enabled, a warning under -Werror, and an error when its diagnostics are
                // errors by default (incompatible-function-pointer-types, int-conversion in C).
                const clang::DiagnosticIDs& ids = *options.Diags->getDiagnosticIDs();
                std::vector<std::string> errors;
                for (const char* warning : kCheckedWarnings)
                {
                    diagnostics.Warnings.push_back(warning);
                    diagnostics.Warnings.push_back(std::string("no-error=") + warning);
                    llvm::SmallVector<clang::diag::kind, 32> members;
                    if (!ids.getDiagnosticsInGroup(clang::diag::Flavor::WarningOrError, warning,
                                                   members) &&
                        std::all_of(members.begin(), members.end(), [&](clang::diag::kind id)
                                    { return ids.isDefaultMappingAsError(id); }))
                    {
                        errors.push_back(std::string("error=") + warning);
                    }
                }
                diagnostics.Warnings.insert(diagnostics.Warnings.end(), errors.begin(),
                                            errors.end());
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

    bool const_suggestion::SiteReader::edited(const SourcePosition& at) const
    {
        const auto edits = edits_.find(at.file);
        if (edits == edits_.end())
            return false;
        return std::any_of(edits->second.begin(), edits->second.end(),
                           [&](const Edit& edit)
                           {
                               return edit.offset <= at.offset &&
                                      at.offset <
                                          edit.offset + std::max<std::size_t>(edit.length, 1);
                           });
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
