// SPDX-License-Identifier: Apache-2.0
#include "analysis/BufferWriteModel.hpp"
#include "mangle.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>

#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>

namespace ctrace::stack::analysis
{
    namespace
    {
        static std::string trimCopy(const std::string& input)
        {
            std::size_t begin = 0;
            while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
                ++begin;
            std::size_t end = input.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
                --end;
            return input.substr(begin, end - begin);
        }

        static bool parseUnsignedIndex(const std::string& token, unsigned& out)
        {
            if (token.empty())
                return false;
            unsigned value = 0;
            for (char c : token)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    return false;
                const unsigned digit = static_cast<unsigned>(c - '0');
                if (value > (std::numeric_limits<unsigned>::max() - digit) / 10u)
                    return false;
                value = value * 10u + digit;
            }
            out = value;
            return true;
        }

        static bool globMatches(llvm::StringRef pattern, llvm::StringRef text)
        {
            std::size_t p = 0;
            std::size_t t = 0;
            std::size_t star = llvm::StringRef::npos;
            std::size_t match = 0;

            while (t < text.size())
            {
                if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
                {
                    ++p;
                    ++t;
                    continue;
                }
                if (p < pattern.size() && pattern[p] == '*')
                {
                    star = p++;
                    match = t;
                    continue;
                }
                if (star != llvm::StringRef::npos)
                {
                    p = star + 1;
                    t = ++match;
                    continue;
                }
                return false;
            }

            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            return p == pattern.size();
        }

        static bool hasUnsupportedBracketClassSyntax(llvm::StringRef pattern)
        {
            return pattern.contains('[') || pattern.contains(']');
        }
    } // namespace

    bool parseBufferWriteModel(const std::string& path, BufferWriteModel& out, std::string& error)
    {
        std::ifstream in(path);
        if (!in)
        {
            error = "cannot open buffer model file: " + path;
            return false;
        }

        out.rules.clear();
        std::string line;
        unsigned lineNo = 0;
        while (std::getline(in, line))
        {
            ++lineNo;
            const std::size_t hashPos = line.find('#');
            if (hashPos != std::string::npos)
                line.erase(hashPos);
            line = trimCopy(line);
            if (line.empty())
                continue;

            std::istringstream iss(line);
            std::vector<std::string> tokens;
            std::string tok;
            while (iss >> tok)
                tokens.push_back(tok);
            if (tokens.empty())
                continue;

            BufferWriteRule rule;
            if (tokens[0] == "bounded_write")
            {
                if (tokens.size() != 4)
                {
                    error = "invalid bounded_write rule at line " + std::to_string(lineNo);
                    return false;
                }
                if (hasUnsupportedBracketClassSyntax(tokens[1]))
                {
                    error = "unsupported character class syntax '[...]' at line " +
                            std::to_string(lineNo) +
                            " (buffer model supports only '*' and '?' wildcards)";
                    return false;
                }

                unsigned destArgIndex = 0;
                unsigned sizeArgIndex = 0;
                if (!parseUnsignedIndex(tokens[2], destArgIndex) ||
                    !parseUnsignedIndex(tokens[3], sizeArgIndex))
                {
                    error = "invalid argument index at line " + std::to_string(lineNo);
                    return false;
                }
                rule.kind = BufferWriteRuleKind::BoundedWrite;
                rule.functionPattern = tokens[1];
                rule.destArgIndex = destArgIndex;
                rule.sizeArgIndex = sizeArgIndex;
            }
            else if (tokens[0] == "unbounded_write")
            {
                if (tokens.size() != 3)
                {
                    error = "invalid unbounded_write rule at line " + std::to_string(lineNo);
                    return false;
                }
                if (hasUnsupportedBracketClassSyntax(tokens[1]))
                {
                    error = "unsupported character class syntax '[...]' at line " +
                            std::to_string(lineNo) +
                            " (buffer model supports only '*' and '?' wildcards)";
                    return false;
                }

                unsigned destArgIndex = 0;
                if (!parseUnsignedIndex(tokens[2], destArgIndex))
                {
                    error = "invalid argument index at line " + std::to_string(lineNo);
                    return false;
                }
                rule.kind = BufferWriteRuleKind::UnboundedWrite;
                rule.functionPattern = tokens[1];
                rule.destArgIndex = destArgIndex;
            }
            else
            {
                error = "unknown buffer model action '" + tokens[0] + "' at line " +
                        std::to_string(lineNo);
                return false;
            }

            out.rules.push_back(std::move(rule));
        }

        return true;
    }

    const BufferWriteRuleMatcher::NameVariants&
    BufferWriteRuleMatcher::namesFor(const llvm::Function& callee)
    {
        auto it = namesCache.find(&callee);
        if (it != namesCache.end())
            return it->second;

        NameVariants variants;
        variants.mangled = callee.getName().str();
        variants.demangled = ctrace_tools::demangle(variants.mangled.c_str());
        variants.demangledBase = variants.demangled;
        if (const std::size_t pos = variants.demangledBase.find('('); pos != std::string::npos)
            variants.demangledBase = variants.demangledBase.substr(0, pos);

        auto [insertedIt, _] = namesCache.emplace(&callee, std::move(variants));
        return insertedIt->second;
    }

    bool BufferWriteRuleMatcher::ruleMatchesFunction(const BufferWriteRule& rule,
                                                     const llvm::Function& callee)
    {
        const NameVariants& names = namesFor(callee);
        const llvm::StringRef pattern(rule.functionPattern);
        const bool hasGlob = pattern.contains('*') || pattern.contains('?');
        if (!hasGlob)
        {
            return rule.functionPattern == names.mangled ||
                   rule.functionPattern == names.demangled ||
                   rule.functionPattern == names.demangledBase;
        }
        return globMatches(pattern, names.mangled) || globMatches(pattern, names.demangled) ||
               globMatches(pattern, names.demangledBase);
    }

    const BufferWriteRule* BufferWriteRuleMatcher::findMatchingRule(const BufferWriteModel& model,
                                                                    const llvm::Function& callee,
                                                                    std::size_t argCount)
    {
        for (const BufferWriteRule& rule : model.rules)
        {
            if (!ruleMatchesFunction(rule, callee))
                continue;
            if (rule.kind == BufferWriteRuleKind::BoundedWrite)
            {
                if (rule.destArgIndex >= argCount || rule.sizeArgIndex >= argCount)
                    continue;
            }
            else
            {
                if (rule.destArgIndex >= argCount)
                    continue;
            }
            return &rule;
        }
        return nullptr;
    }
} // namespace ctrace::stack::analysis
