// SPDX-License-Identifier: Apache-2.0
// Comparison of the code generated before and after a const rewrite (#217).
#include "analysis/ConstSuggestionCheck.hpp"
#include "ConstSuggestionInternal.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        bool isSymbolChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '$' ||
                   c == '.' || c == '_';
        }

        // The symbol written at text[at] ('@' or '$' excluded), quoted or not, and its length.
        std::pair<std::string, std::size_t> symbolAt(const std::string& text, std::size_t at)
        {
            if (at < text.size() && text[at] == '"')
            {
                const std::size_t end = text.find('"', at + 1);
                if (end == std::string::npos)
                    return {"", 0};
                return {text.substr(at + 1, end - at - 1), end - at + 1};
            }
            std::size_t end = at;
            while (end < text.size() && isSymbolChar(text[end]))
                ++end;
            return {text.substr(at, end - at), end - at};
        }

        std::string spellSymbol(const std::string& name)
        {
            const bool plain = !name.empty() &&
                               std::all_of(name.begin(), name.end(), isSymbolChar) &&
                               !std::isdigit(static_cast<unsigned char>(name.front()));
            return plain ? name : "\"" + name + "\"";
        }

        // A printed line without its comment: the predecessors a block label lists follow the
        // order of uses, which copying a module changes, and say nothing an execution does.
        llvm::StringRef withoutComment(llvm::StringRef line)
        {
            bool quoted = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                if (line[i] == '"')
                    quoted = !quoted;
                else if (line[i] == ';' && !quoted)
                    return line.take_front(i).rtrim();
            }
            return line;
        }

        using const_suggestion::ModuleText;

        // Removes what LLVM says carries no meaning for an execution, so that every other
        // metadata can be compared by contents:
        // - debug information (StripDebugInfo, which also removes heapallocsite, a debug type,
        //   and the debug locations of loop identifiers);
        // - srcloc, the source position of inline assembly and dontcall diagnostics;
        // - annotation, which "does not impact the semantics of the program";
        // - the descriptive string of an alias scope or domain identified by a self-reference.
        void removeNonSemanticMetadata(llvm::Module& module)
        {
            llvm::StripDebugInfo(module);
            llvm::LLVMContext& context = module.getContext();
            const unsigned srcloc = context.getMDKindID("srcloc");
            llvm::MDBuilder builder(context);
            std::map<const llvm::MDNode*, llvm::MDNode*> unnamed;
            const auto selfNamed = [](const llvm::MDNode* node)
            { return node->getNumOperands() > 0 && node->getOperand(0) == node; };
            std::function<llvm::MDNode*(llvm::MDNode*)> domain = [&](llvm::MDNode* node)
            {
                if (!selfNamed(node))
                    return node;
                llvm::MDNode*& out = unnamed[node];
                if (!out)
                    out = builder.createAnonymousAliasScopeDomain();
                return out;
            };
            const auto scope = [&](llvm::MDNode* node)
            {
                auto* parent = node->getNumOperands() > 1
                                   ? llvm::dyn_cast<llvm::MDNode>(node->getOperand(1).get())
                                   : nullptr;
                if (!selfNamed(node) || !parent)
                    return node;
                llvm::MDNode*& out = unnamed[node];
                if (!out)
                    out = builder.createAnonymousAliasScope(domain(parent));
                return out;
            };
            const auto list = [&](llvm::MDNode* node)
            {
                llvm::SmallVector<llvm::Metadata*> scopes;
                for (const llvm::MDOperand& operand : node->operands())
                {
                    auto* member = llvm::dyn_cast_or_null<llvm::MDNode>(operand.get());
                    scopes.push_back(member ? scope(member) : operand.get());
                }
                return llvm::MDNode::get(context, scopes);
            };
            for (llvm::Function& function : module)
            {
                for (llvm::Instruction& instruction : llvm::instructions(function))
                {
                    instruction.setMetadata(srcloc, nullptr);
                    instruction.setMetadata(llvm::LLVMContext::MD_annotation, nullptr);
                    for (const unsigned kind :
                         {llvm::LLVMContext::MD_alias_scope, llvm::LLVMContext::MD_noalias})
                    {
                        if (llvm::MDNode* scopes = instruction.getMetadata(kind))
                            instruction.setMetadata(kind, list(scopes));
                    }
                    auto* declaration = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
                    if (declaration && declaration->getIntrinsicID() ==
                                           llvm::Intrinsic::experimental_noalias_scope_decl)
                    {
                        auto* operand =
                            llvm::cast<llvm::MetadataAsValue>(declaration->getArgOperand(0));
                        if (auto* scopes = llvm::dyn_cast<llvm::MDNode>(operand->getMetadata()))
                        {
                            declaration->setArgOperand(
                                0, llvm::MetadataAsValue::get(context, list(scopes)));
                        }
                    }
                }
            }
        }

        ModuleText textOf(llvm::Module& module)
        {
            removeNonSemanticMetadata(module);
            std::string printed;
            llvm::raw_string_ostream os(printed);
            module.print(os, nullptr);
            os.flush();

            ModuleText out;
            std::string current;
            std::string body;
            for (llvm::StringRef printedLine : llvm::split(printed, '\n'))
            {
                const llvm::StringRef line = withoutComment(printedLine);
                if (!current.empty())
                {
                    body += line.str() + "\n";
                    if (line == "}")
                    {
                        out.globals[current] = body;
                        current.clear();
                    }
                    continue;
                }
                // Metadata: compared by contents where they are used, named ones apart.
                if (line.starts_with("!"))
                {
                    const std::size_t equal = line.find(" = ");
                    if (equal != llvm::StringRef::npos)
                    {
                        const std::string name = line.substr(0, equal).str();
                        const bool numbered =
                            name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]));
                        (numbered ? out.metadata : out.namedMetadata)[name] =
                            line.substr(equal + 3).str();
                    }
                    continue;
                }
                // Comdat selections are left out: each definition names its comdat, renamed and
                // compared with it.
                if (line.empty() || line.starts_with(";") || line.starts_with("$") ||
                    line.starts_with("source_filename") || line.starts_with("target "))
                {
                    continue;
                }
                if (line.starts_with("attributes #"))
                {
                    const std::size_t equal = line.find(" = ");
                    out.attributeGroups[line.substr(11, equal - 11).str()] =
                        line.substr(equal + 3).str();
                    continue;
                }
                if (line.starts_with("%"))
                {
                    const std::size_t equal = line.find(" = ");
                    out.types[line.substr(0, equal).str()] = line.str();
                    continue;
                }
                const bool definition = line.starts_with("define ");
                if (definition || line.starts_with("declare "))
                {
                    const std::size_t at = line.find('@');
                    const std::string name = symbolAt(line.str(), at + 1).first;
                    if (definition)
                    {
                        current = name;
                        body = line.str() + "\n";
                        out.defined.insert(name);
                    }
                    else
                    {
                        out.globals[name] = line.str();
                    }
                    continue;
                }
                if (line.starts_with("@"))
                {
                    const std::string name = symbolAt(line.str(), 1).first;
                    out.globals[name] = line.str();
                    if (!line.contains("= external ") && !line.contains("= extern_weak ") &&
                        !line.contains("= dllimport external "))
                    {
                        out.defined.insert(name);
                    }
                    continue;
                }
                // Module-level assembly.
                out.globals[line.str()] = line.str();
            }
            return out;
        }

        // Rewrites `text` symbol by symbol and attribute group by contents. Each metadata it uses
        // is numbered by first use and its definition appended, so metadata are compared by
        // contents and by the places that use them, not by their numbers. Quoted strings are left
        // alone.
        std::string normalized(const std::string& text,
                               const std::map<std::string, std::string>& rename,
                               const std::map<std::string, std::string>& attributeGroups,
                               const std::map<std::string, std::string>& metadata)
        {
            std::string out;
            std::map<std::string, std::size_t> numbers;
            std::vector<std::string> used;
            const auto scan = [&](const std::string& piece)
            {
                bool quoted = false;
                for (std::size_t i = 0; i < piece.size();)
                {
                    const char c = piece[i];
                    if (c == '"' && !(i > 0 && piece[i - 1] == '@'))
                    {
                        quoted = !quoted;
                        out += c;
                        ++i;
                        continue;
                    }
                    if (quoted)
                    {
                        out += c;
                        ++i;
                        continue;
                    }
                    if (c == '@' || c == '$')
                    {
                        const auto [name, length] = symbolAt(piece, i + 1);
                        if (length != 0)
                        {
                            const auto renamed = rename.find(name);
                            out += c;
                            out += spellSymbol(renamed == rename.end() ? name : renamed->second);
                            i += 1 + length;
                            continue;
                        }
                    }
                    const bool numbered = i + 1 < piece.size() &&
                                          std::isdigit(static_cast<unsigned char>(piece[i + 1]));
                    if ((c == '!' || c == '#') && numbered)
                    {
                        std::size_t end = i + 1;
                        while (end < piece.size() &&
                               std::isdigit(static_cast<unsigned char>(piece[end])))
                            ++end;
                        const std::string reference = piece.substr(i, end - i);
                        if (c == '!')
                        {
                            const auto [it, first] = numbers.try_emplace(reference, numbers.size());
                            if (first)
                                used.push_back(reference);
                            out += "!m" + std::to_string(it->second);
                        }
                        else
                        {
                            const auto group = attributeGroups.find(reference);
                            out += group == attributeGroups.end() ? reference : "#" + group->second;
                        }
                        i = end;
                        continue;
                    }
                    out += c;
                    ++i;
                }
            };
            scan(text);
            for (std::size_t i = 0; i < used.size(); ++i)
            {
                out += "\n!m" + std::to_string(i) + " = ";
                const auto definition = metadata.find(used[i]);
                scan(definition == metadata.end() ? "?" : definition->second);
            }
            return out;
        }

        // The contents of a string constant defined on `line`, and where they start and end.
        bool stringConstant(const std::string& line, std::size_t& begin, std::size_t& end)
        {
            const std::size_t marker = line.find(" c\"");
            if (marker == std::string::npos || line.find(" constant [") == std::string::npos)
                return false;
            begin = marker + 3;
            end = line.find("\\00\"", begin);
            return end != std::string::npos;
        }

        std::string escaped(llvm::StringRef text)
        {
            std::string out;
            llvm::raw_string_ostream os(out);
            llvm::printEscapedString(text, os);
            os.flush();
            return out;
        }

        // `line` with the contents and length of its string constant replaced.
        std::string withString(const std::string& line, std::size_t begin, std::size_t end,
                               const std::string& contents, std::size_t length)
        {
            std::string out = line.substr(0, begin) + contents + line.substr(end);
            const std::size_t array = out.find(" constant [");
            const std::size_t times = out.find(" x i8]", array);
            if (array != std::string::npos && times != std::string::npos)
                out.replace(array + 11, times - array - 11, std::to_string(length + 1));
            return out;
        }

        std::size_t unescapedLength(llvm::StringRef escapedText)
        {
            std::size_t length = 0;
            for (std::size_t i = 0; i < escapedText.size(); ++i, ++length)
            {
                if (escapedText[i] == '\\')
                    i += 2;
            }
            return length;
        }

        std::string firstDifference(const std::string& before, const std::string& after)
        {
            const auto a = llvm::to_vector(llvm::split(before, '\n'));
            const auto b = llvm::to_vector(llvm::split(after, '\n'));
            std::size_t i = 0;
            while (i < a.size() && i < b.size() && a[i] == b[i])
                ++i;
            const std::string was = i < a.size() ? a[i].trim().str() : "(end)";
            const std::string now = i < b.size() ? b[i].trim().str() : "(end)";
            return "'" + was.substr(0, 160) + "' became '" + now.substr(0, 160) + "'";
        }

        // The function template `symbol` is a specialization of, as demangled names write it
        // ("ns::f<"), or "" for a symbol that is no function template specialization.
        std::string templateOf(const std::string& symbol)
        {
            llvm::ItaniumPartialDemangler demangler;
            if (demangler.partialDemangle(symbol.c_str()) || !demangler.isFunction())
                return "";
            const auto text = [](char* buffer)
            {
                std::string out = buffer ? buffer : "";
                std::free(buffer);
                return out;
            };
            std::size_t size = 0;
            const std::string base = text(demangler.getFunctionBaseName(nullptr, &size));
            const std::string scope = text(demangler.getFunctionDeclContextName(nullptr, &size));
            const std::string name = text(demangler.getFunctionName(nullptr, &size));
            const std::string qualified = scope.empty() ? base : scope + "::" + base;
            if (base.empty() || !llvm::StringRef(name).starts_with(qualified + "<"))
                return "";
            return qualified + "<";
        }

        // Whether the demangled `symbol` names `templateName` ("ns::f<") as a whole name.
        bool namesTemplate(const std::string& symbol, const std::string& templateName)
        {
            const std::string text = llvm::demangle(symbol);
            for (std::size_t at = text.find(templateName); at != std::string::npos;
                 at = text.find(templateName, at + 1))
            {
                if (at == 0 || (!std::isalnum(static_cast<unsigned char>(text[at - 1])) &&
                                text[at - 1] != '_'))
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace

    std::string const_suggestion::withoutConst(const std::string& symbol)
    {
        const std::string text = llvm::demangle(symbol);
        std::string out;
        // An identifier is made of letters, digits and underscores: const_iterator is no
        // qualifier.
        const auto inWord = [&](std::size_t i)
        { return std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'; };
        for (std::size_t i = 0; i < text.size();)
        {
            const bool wordStart = i == 0 || !inWord(i - 1);
            if (wordStart && text.compare(i, 5, "const") == 0 &&
                (i + 5 == text.size() || !inWord(i + 5)))
            {
                i += 5;
                continue;
            }
            if (text[i] != ' ')
                out += text[i];
            ++i;
        }
        std::size_t pos = 0;
        while ((pos = out.find("&&", pos)) != std::string::npos)
            out.replace(pos, 2, "&");
        return out;
    }

    std::string const_suggestion::variantOf(const std::string& symbol)
    {
        std::string variant;
        for (std::size_t i = 1; i + 1 < symbol.size(); ++i)
        {
            const char kind = symbol[i];
            const char digit = symbol[i + 1];
            const bool afterName =
                std::isalnum(static_cast<unsigned char>(symbol[i - 1])) || symbol[i - 1] == '_';
            const bool next =
                i + 2 == symbol.size() || std::isupper(static_cast<unsigned char>(symbol[i + 2]));
            if (afterName && next &&
                ((kind == 'C' && digit >= '1' && digit <= '5') ||
                 (kind == 'D' && digit >= '0' && digit <= '5')))
            {
                variant += symbol.substr(i, 2);
            }
        }
        const std::string demangled = llvm::demangle(symbol);
        if (llvm::StringRef(demangled).rtrim().ends_with(" const"))
            variant += "|const";
        return variant;
    }

    const_suggestion::ModuleText const_suggestion::prepare(llvm::Module& module)
    {
        return textOf(module);
    }

    std::optional<std::string> rewrittenCodeDifference(llvm::Module& before, llvm::Module& after,
                                                       llvm::StringRef rewritten,
                                                       const PrintedSignature* signature,
                                                       std::vector<std::string>* accepted,
                                                       std::string* renamedTo)
    {
        return const_suggestion::codeDifference(textOf(before), textOf(after), rewritten, signature,
                                                accepted, renamedTo);
    }

    std::optional<std::string>
    const_suggestion::codeDifference(const ModuleText& was, const ModuleText& now,
                                     llvm::StringRef rewritten, const PrintedSignature* signature,
                                     std::vector<std::string>* accepted, std::string* renamedTo)
    {
        const std::string function = rewritten.str();

        std::vector<std::string> removed;
        std::vector<std::string> added;
        for (const auto& [name, text] : was.globals)
        {
            if (!now.globals.count(name))
                removed.push_back(name);
        }
        for (const auto& [name, text] : now.globals)
        {
            if (!was.globals.count(name))
                added.push_back(name);
        }

        // The rewritten function and what is named inside it change their mangled names: pair
        // them by their names with the function's encoding masked, then by their demangled names
        // without const.
        const std::string functionKey = withoutConst(function);
        std::vector<std::string> encodings;
        if (rewritten.starts_with("_Z"))
            encodings.push_back(function.substr(2));
        for (const std::string& name : added)
        {
            if (name.starts_with("_Z") && withoutConst(name) == functionKey)
                encodings.push_back(name.substr(2));
        }
        std::sort(encodings.begin(), encodings.end(),
                  [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        const auto masked = [&](std::string name)
        {
            for (const std::string& encoding : encodings)
            {
                for (std::size_t at = name.find(encoding); at != std::string::npos;
                     at = name.find(encoding, at + 3))
                {
                    name.replace(at, encoding.size(), "@F@");
                }
            }
            // Substitution indices (S_, S0_, ...) depend on what precedes them.
            std::string out;
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                if (name[i] == 'S')
                {
                    std::size_t end = i + 1;
                    while (end < name.size() &&
                           (std::isdigit(static_cast<unsigned char>(name[end])) ||
                            std::isupper(static_cast<unsigned char>(name[end]))))
                        ++end;
                    if (end < name.size() && name[end] == '_')
                    {
                        out += "S_";
                        i = end;
                        continue;
                    }
                }
                out += name[i];
            }
            return out;
        };

        std::map<std::string, std::set<std::string>> byMasked;
        std::map<std::string, std::set<std::string>> byDemangled;
        for (const std::string& name : added)
        {
            byMasked[masked(name)].insert(name);
            byDemangled[withoutConst(name)].insert(name);
        }
        std::map<std::string, std::string> rename;
        std::set<std::string> taken;
        std::vector<std::string> unpaired;
        for (const std::string& name : removed)
        {
            bool paired = false;
            for (const auto* table : {&byMasked, &byDemangled})
            {
                const auto found =
                    table->find(table == &byMasked ? masked(name) : withoutConst(name));
                if (found == table->end())
                    continue;
                std::vector<std::string> candidates;
                for (const std::string& candidate : found->second)
                {
                    if (!taken.count(candidate))
                        candidates.push_back(candidate);
                }
                if (candidates.size() > 1)
                {
                    std::erase_if(candidates, [&](const std::string& candidate)
                                  { return variantOf(candidate) != variantOf(name); });
                }
                if (candidates.size() != 1)
                    continue;
                // A symbol the unit only declares has no code to compare: it may be paired only
                // if it is the rewritten function itself.
                const bool itself = name == function || withoutConst(name) == functionKey ||
                                    masked(name) == "_Z@F@";
                if (!itself && (!was.defined.count(name) || !now.defined.count(candidates[0])))
                    break;
                rename[name] = candidates[0];
                taken.insert(candidates[0]);
                paired = true;
                break;
            }
            if (!paired)
                unpaired.push_back(name);
        }

        // Rewritten as const T&, a forwarding reference T&& deduces G instead of G& or const G&
        // for an lvalue argument: the specializations of the rewritten template that change this
        // way, and what is named inside them, change their names beyond const. One is paired
        // when its demangled name without const and references is carried by exactly one symbol
        // before the rewrite and one after, both with code to compare.
        const std::string templateName = templateOf(function);
        if (!templateName.empty())
        {
            const auto loose = [](const std::string& name)
            {
                std::string key = withoutConst(name);
                std::erase(key, '&');
                return key + "|" + variantOf(name);
            };
            std::map<std::string, std::vector<std::string>> before;
            std::map<std::string, std::vector<std::string>> after;
            for (const std::string& name : unpaired)
            {
                if (namesTemplate(name, templateName))
                    before[loose(name)].push_back(name);
            }
            for (const std::string& name : added)
            {
                if (!taken.count(name) && namesTemplate(name, templateName))
                    after[loose(name)].push_back(name);
            }
            for (const auto& [key, names] : before)
            {
                const auto other = after.find(key);
                if (names.size() != 1 || other == after.end() || other->second.size() != 1 ||
                    !was.defined.count(names[0]) || !now.defined.count(other->second[0]))
                {
                    continue;
                }
                rename[names[0]] = other->second[0];
                taken.insert(other->second[0]);
                std::erase(unpaired, names[0]);
            }
        }

        std::vector<std::string> unexpected;
        for (const std::string& name : added)
        {
            if (!taken.count(name))
                unexpected.push_back(name);
        }
        if (!unpaired.empty() || !unexpected.empty())
        {
            const auto list = [](const std::vector<std::string>& names)
            { return names.empty() ? std::string("none") : llvm::join(names, ", "); };
            return "symbols no longer used: " + list(unpaired) +
                   "; symbols used only after the rewrite: " + list(unexpected);
        }
        if (renamedTo)
        {
            const auto found = rename.find(function);
            *renamedTo = found == rename.end() ? function : found->second;
        }

        if (was.types != now.types)
        {
            for (const auto& [name, definition] : was.types)
            {
                const auto other = now.types.find(name);
                if (other == now.types.end() || other->second != definition)
                    return "type " + name + " changed";
            }
            return "types added by the rewrite";
        }

        bool signatureAccepted = false;
        for (const auto& [name, text] : was.globals)
        {
            const auto renamed = rename.find(name);
            const std::string newName = renamed == rename.end() ? name : renamed->second;
            const std::string expected =
                normalized(text, rename, was.attributeGroups, was.metadata);
            std::string actual =
                normalized(now.globals.at(newName), {}, now.attributeGroups, now.metadata);
            if (expected == actual)
                continue;

            std::size_t begin = 0;
            std::size_t end = 0;
            std::size_t newBegin = 0;
            std::size_t newEnd = 0;
            if (stringConstant(expected, begin, end) && stringConstant(actual, newBegin, newEnd))
            {
                const std::string oldText = expected.substr(begin, end - begin);
                const std::string newText = actual.substr(newBegin, newEnd - newBegin);
                // A type name holds the mangled name of its type, renamed with the function.
                const std::string prefix = oldText.starts_with("*") ? "*" : "";
                const bool typeName = name.starts_with("_ZTS") && renamed != rename.end() &&
                                      oldText == prefix + name.substr(4) &&
                                      newText == prefix + newName.substr(4);
                // A string whose only change is the function's printed signature.
                bool printedSignature = false;
                if (!typeName && signature && signature->before != signature->after)
                {
                    const std::string from = escaped(signature->before);
                    const std::string to = escaped(signature->after);
                    std::string replaced = oldText;
                    for (std::size_t at = from.empty() ? std::string::npos : replaced.find(from);
                         at != std::string::npos; at = replaced.find(from, at + to.size()))
                    {
                        replaced.replace(at, from.size(), to);
                        printedSignature = true;
                    }
                    printedSignature = printedSignature && replaced == newText;
                }
                if (typeName || printedSignature)
                {
                    actual =
                        withString(actual, newBegin, newEnd, oldText, unescapedLength(oldText));
                    if (actual == expected)
                    {
                        if (accepted && typeName)
                        {
                            accepted->push_back("difference: the name typeid gives a type named "
                                                "after the function changes, from \"" +
                                                oldText + "\" to \"" + newText + "\"");
                        }
                        if (accepted && printedSignature && !signatureAccepted)
                        {
                            accepted->push_back("difference: the signature printed by "
                                                "__PRETTY_FUNCTION__ changes, from \"" +
                                                signature->before + "\" to \"" + signature->after +
                                                "\"");
                        }
                        signatureAccepted = signatureAccepted || printedSignature;
                        continue;
                    }
                }
            }
            return "code changed in " + name + ": " + firstDifference(expected, actual);
        }
        for (const auto& [name, operands] : was.namedMetadata)
        {
            const auto other = now.namedMetadata.find(name);
            const std::string expected =
                normalized(operands, rename, was.attributeGroups, was.metadata);
            const std::string actual =
                other == now.namedMetadata.end()
                    ? "(none)"
                    : normalized(other->second, {}, now.attributeGroups, now.metadata);
            if (expected != actual)
                return "metadata " + name + " changed: " + firstDifference(expected, actual);
        }
        for (const auto& [name, operands] : now.namedMetadata)
        {
            if (!was.namedMetadata.count(name))
                return "metadata " + name + " only after the rewrite";
        }
        return std::nullopt;
    }
} // namespace ctrace::stack::analysis
