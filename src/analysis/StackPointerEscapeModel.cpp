#include "StackPointerEscapeInternal.hpp"
#include "mangle.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>

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
            {
                ++begin;
            }
            std::size_t end = input.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
            {
                --end;
            }
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

    bool parseStackEscapeModel(const std::string& path, StackEscapeModel& out, std::string& error)
    {
        std::ifstream in(path);
        if (!in)
        {
            error = "cannot open stack escape model file: " + path;
            return false;
        }

        out.noEscapeArgRules.clear();
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

            if (tokens[0] != "noescape_arg")
            {
                error = "unknown stack escape model action '" + tokens[0] + "' at line " +
                        std::to_string(lineNo);
                return false;
            }
            if (tokens.size() != 3)
            {
                error = "invalid noescape_arg rule at line " + std::to_string(lineNo);
                return false;
            }
            if (hasUnsupportedBracketClassSyntax(tokens[1]))
            {
                error = "unsupported character class syntax '[...]' at line " +
                        std::to_string(lineNo) +
                        " (stack escape model supports only '*' and '?' wildcards)";
                return false;
            }

            unsigned argIndex = 0;
            if (!parseUnsignedIndex(tokens[2], argIndex))
            {
                error = "invalid argument index at line " + std::to_string(lineNo);
                return false;
            }

            StackEscapeRule rule;
            rule.functionPattern = tokens[1];
            rule.argIndex = argIndex;
            out.noEscapeArgRules.push_back(std::move(rule));
        }

        return true;
    }

    bool callParamHasNonCaptureLikeAttr(const llvm::CallBase& CB, unsigned argIndex)
    {
        return CB.paramHasAttr(argIndex, llvm::Attribute::NoCapture) ||
               CB.paramHasAttr(argIndex, llvm::Attribute::ByVal) ||
               CB.paramHasAttr(argIndex, llvm::Attribute::ByRef);
    }

    bool isStdLibCallee(const llvm::Function* F)
    {
        if (!F)
            return false;

        const llvm::StringRef name = F->getName();
        return name.starts_with("_ZNSt3__1") || name.starts_with("_ZSt") ||
               name.starts_with("_ZNSt") || name.starts_with("__cxx");
    }

    const StackEscapeRuleMatcher::NameVariants&
    StackEscapeRuleMatcher::namesFor(const llvm::Function& callee)
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

    bool StackEscapeRuleMatcher::modelRuleMatchesFunction(const StackEscapeRule& rule,
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

    bool StackEscapeRuleMatcher::modelSaysNoEscapeArg(const StackEscapeModel& model,
                                                      const llvm::Function* callee,
                                                      unsigned argIndex)
    {
        if (!callee)
            return false;
        for (const StackEscapeRule& rule : model.noEscapeArgRules)
        {
            if (rule.argIndex != argIndex)
                continue;
            if (modelRuleMatchesFunction(rule, *callee))
                return true;
        }
        return false;
    }
} // namespace ctrace::stack::analysis
