// SPDX-License-Identifier: Apache-2.0
// Comparison of the code generated before and after a const rewrite (#217).
#include "analysis/ConstSuggestionCheck.hpp"
#include "ConstSuggestionInternal.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/StringExtras.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DebugInfo.h>
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

        // The text of one module, keyed by what each piece defines.
        struct ModuleText
        {
            std::map<std::string, std::string> globals; // functions, variables, aliases
            std::map<std::string, std::string> types;
            std::map<std::string, std::string> attributeGroups; // "#N" -> "{ ... }"
            std::set<std::string> defined;                      // has code or data here
        };

        ModuleText textOf(llvm::Module& module)
        {
            llvm::StripDebugInfo(module);
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
                // Comdat selections are left out: each definition names its comdat, renamed and
                // compared with it.
                if (line.empty() || line.starts_with(";") || line.starts_with("!") ||
                    line.starts_with("$") || line.starts_with("source_filename") ||
                    line.starts_with("target "))
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

        // Rewrites `text` symbol by symbol, attribute group by contents, and numbers its
        // metadata by first use. Quoted strings are left alone.
        std::string normalized(const std::string& text,
                               const std::map<std::string, std::string>& rename,
                               const std::map<std::string, std::string>& attributeGroups)
        {
            std::string out;
            std::map<std::string, std::size_t> metadata;
            bool quoted = false;
            for (std::size_t i = 0; i < text.size();)
            {
                const char c = text[i];
                if (c == '"' && !(i > 0 && text[i - 1] == '@'))
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
                    const auto [name, length] = symbolAt(text, i + 1);
                    if (length != 0)
                    {
                        const auto renamed = rename.find(name);
                        out += c;
                        out += spellSymbol(renamed == rename.end() ? name : renamed->second);
                        i += 1 + length;
                        continue;
                    }
                }
                if (c == '!' && i + 1 < text.size() &&
                    std::isdigit(static_cast<unsigned char>(text[i + 1])))
                {
                    std::size_t end = i + 1;
                    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                        ++end;
                    const auto [it, inserted] =
                        metadata.try_emplace(text.substr(i, end - i), metadata.size());
                    out += "!m" + std::to_string(it->second);
                    i = end;
                    continue;
                }
                if (c == '#' && i + 1 < text.size() &&
                    std::isdigit(static_cast<unsigned char>(text[i + 1])))
                {
                    std::size_t end = i + 1;
                    while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end])))
                        ++end;
                    const auto group = attributeGroups.find(text.substr(i, end - i));
                    out += group == attributeGroups.end() ? text.substr(i, end - i)
                                                          : "#" + group->second;
                    i = end;
                    continue;
                }
                out += c;
                ++i;
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
    } // namespace

    std::string const_suggestion::withoutConst(const std::string& symbol)
    {
        const std::string text = llvm::demangle(symbol);
        std::string out;
        for (std::size_t i = 0; i < text.size();)
        {
            const bool wordStart = i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1]));
            if (wordStart && text.compare(i, 5, "const") == 0 &&
                (i + 5 == text.size() || !std::isalnum(static_cast<unsigned char>(text[i + 5]))))
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

    std::optional<std::string> rewrittenCodeDifference(llvm::Module& before, llvm::Module& after,
                                                       llvm::StringRef rewritten,
                                                       const PrintedSignature* signature,
                                                       std::vector<std::string>* accepted,
                                                       std::string* renamedTo)
    {
        using const_suggestion::variantOf;
        using const_suggestion::withoutConst;
        const ModuleText was = textOf(before);
        const ModuleText now = textOf(after);
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
            const std::string expected = normalized(text, rename, was.attributeGroups);
            std::string actual = normalized(now.globals.at(newName), {}, now.attributeGroups);
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
        return std::nullopt;
    }
} // namespace ctrace::stack::analysis
