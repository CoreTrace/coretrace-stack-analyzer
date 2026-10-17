// SPDX-License-Identifier: Apache-2.0
// Plans the Clang check of a const suggestion (#217, §5.5): the parameter, the type it must get,
// and the edits that write that type in every declaration the defining file sees.
#include "ConstSuggestionInternal.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <clang/Lex/MacroInfo.h>
#include <clang/Lex/PPCallbacks.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::Edit;
        using const_suggestion::MacroDefinitions;
        using const_suggestion::realPath;
        using const_suggestion::SuggestionPlan;
        using const_suggestion::writtenType;

        // Records where each macro definition lies, by file.
        class MacroRecorder : public clang::PPCallbacks
        {
          public:
            MacroRecorder(const clang::SourceManager& sm, MacroDefinitions& out)
                : sm_(sm), out_(out)
            {
            }

            void MacroDefined(const clang::Token&, const clang::MacroDirective* directive) override
            {
                const clang::MacroInfo* info = directive->getMacroInfo();
                const clang::SourceLocation begin = info->getDefinitionLoc();
                const clang::SourceLocation end = info->getDefinitionEndLoc();
                if (!begin.isFileID() || !end.isFileID())
                    return;
                const auto [file, from] = sm_.getDecomposedLoc(begin);
                if (sm_.getFileID(end) == file)
                    out_[file].push_back({from, sm_.getFileOffset(end)});
            }

          private:
            const clang::SourceManager& sm_;
            MacroDefinitions& out_;
        };

        // Whether `offset` of `file` lies in a macro definition, as the preprocessor read it:
        // continuation lines included, whatever spaces follow their backslash.
        bool inMacroDefinition(const MacroDefinitions& macros, clang::FileID file,
                               std::size_t offset)
        {
            const auto found = macros.find(file);
            return found != macros.end() &&
                   std::any_of(found->second.begin(), found->second.end(),
                               [&](const std::pair<unsigned, unsigned>& range)
                               { return range.first <= offset && offset <= range.second; });
        }

        class Planner
        {
          public:
            Planner(clang::ASTContext& context, const MacroDefinitions& macros,
                    SuggestionPlan& plan)
                : context_(context), sm_(context.getSourceManager()), macros_(macros), plan_(plan)
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
                if (!plan_.indeterminate)
                    plan_.indeterminate = std::move(why);
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
                if (inMacroDefinition(macros_, file, offset))
                    return fail("the declaration is written by a macro definition");
                std::vector<Edit>& edits = plan_.edits[realPath(entry->getName())];
                const bool seen = std::any_of(edits.begin(), edits.end(), [&](const Edit& edit)
                                              { return edit.offset == offset; });
                if (!seen)
                    edits.push_back({offset, length, text});
                return true;
            }

            clang::ASTContext& context_;
            const clang::SourceManager& sm_;
            const MacroDefinitions& macros_;
            SuggestionPlan& plan_;
            const clang::ParmVarDecl* param_ = nullptr;
        };

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
    } // namespace

    // A type written canonically. An anonymous type, such as a lambda's, is written without
    // its location: the path of that location depends on how the file was named, relative
    // before the rewrite and absolute once the rewritten file is remapped.
    std::string const_suggestion::writtenType(const clang::ASTContext& context,
                                              clang::QualType type)
    {
        clang::PrintingPolicy policy(context.getLangOpts());
        policy.AnonymousTagLocations = false;
        return type.getCanonicalType().getAsString(policy);
    }

    std::unique_ptr<clang::PPCallbacks>
    const_suggestion::recordMacroDefinitions(const clang::SourceManager& sm, MacroDefinitions& out)
    {
        return std::make_unique<MacroRecorder>(sm, out);
    }

    SuggestionPlan const_suggestion::plan(clang::ASTContext& context,
                                          const MacroDefinitions& macros,
                                          const clang::FunctionDecl& function,
                                          std::optional<unsigned> sourceIndex)
    {
        SuggestionPlan out;
        const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(&function);
        out.declarableElsewhere = method == nullptr && function.isExternallyVisible();
        if (!sourceIndex)
        {
            out.indeterminate = "the parameter is not found in the source";
            return out;
        }
        out.sourceIndex = *sourceIndex;
        if (out.sourceIndex >= function.getNumParams())
        {
            out.indeterminate = "the parameter is not found in the source";
            return out;
        }
        out.expectedType = expectedType(context, function.getParamDecl(out.sourceIndex)->getType());
        if (out.expectedType.empty())
        {
            out.indeterminate = "the parameter is no pointer or reference";
            return out;
        }
        const clang::FunctionDecl* pattern = function.getTemplateInstantiationPattern();
        if (!pattern)
            pattern = &function;
        Planner planner(context, macros, out);
        for (const clang::FunctionDecl* declaration : pattern->redecls())
        {
            if (declaration->isImplicit())
                continue;
            if (out.sourceIndex >= declaration->getNumParams())
            {
                out.indeterminate = "the declarations disagree on the parameters";
                return out;
            }
            if (!planner.plan(*declaration, *declaration->getParamDecl(out.sourceIndex)))
                return out;
        }
        return out;
    }
} // namespace ctrace::stack::analysis
