#include "analysis/AnalyzerUtils.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/DebugInfoMetadata.h>

#include "mangle.hpp"

namespace ctrace::stack::analysis
{
    std::string formatFunctionNameForMessage(const std::string& name)
    {
        if (ctrace_tools::isMangled(name))
            return ctrace_tools::demangle(name.c_str());
        return name;
    }

    std::string getFunctionSourcePath(const llvm::Function& F)
    {
        if (auto* sp = F.getSubprogram())
        {
            if (auto* file = sp->getFile())
            {
                std::string dir = file->getDirectory().str();
                std::string name = file->getFilename().str();
                if (!dir.empty())
                    return dir + "/" + name;
                return name;
            }
        }
        return {};
    }

    bool getFunctionSourceLocation(const llvm::Function& F, unsigned& line, unsigned& column)
    {
        line = 0;
        column = 0;

        for (const llvm::BasicBlock& BB : F)
        {
            for (const llvm::Instruction& I : BB)
            {
                llvm::DebugLoc DL = I.getDebugLoc();
                if (!DL)
                    continue;
                line = DL.getLine();
                column = DL.getCol();
                if (line != 0)
                {
                    if (column == 0)
                        column = 1;
                    return true;
                }
            }
        }

        if (auto* sp = F.getSubprogram())
        {
            line = sp->getLine();
            if (line != 0)
            {
                column = 1;
                return true;
            }
        }

        return false;
    }

    std::string buildMaxStackCallPath(const llvm::Function* F, const CallGraph& CG,
                                      const InternalAnalysisState& state)
    {
        std::string path;
        std::set<const llvm::Function*> visited;
        const llvm::Function* current = F;

        while (current)
        {
            if (!visited.insert(current).second)
                break;

            if (!path.empty())
                path += " -> ";
            path += current->getName().str();

            const llvm::Function* bestCallee = nullptr;
            StackEstimate bestStack{};

            auto itCG = CG.find(current);
            if (itCG == CG.end())
                break;

            for (const llvm::Function* callee : itCG->second)
            {
                auto itTotal = state.TotalStack.find(callee);
                StackEstimate est =
                    (itTotal != state.TotalStack.end()) ? itTotal->second : StackEstimate{};
                if (!bestCallee || est.bytes > bestStack.bytes)
                {
                    bestCallee = callee;
                    bestStack = est;
                }
            }

            if (!bestCallee || bestStack.bytes == 0)
                break;

            current = bestCallee;
        }

        return path;
    }

    static std::string normalizePathForMatch(const std::string& input)
    {
        if (input.empty())
            return {};

        std::string adjusted = input;
        for (char& c : adjusted)
        {
            if (c == '\\')
                c = '/';
        }

        std::filesystem::path path(adjusted);
        std::error_code ec;
        std::filesystem::path absPath = std::filesystem::absolute(path, ec);
        if (ec)
            absPath = path;

        std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absPath, ec);
        std::filesystem::path norm = ec ? absPath.lexically_normal() : canonicalPath;
        std::string out = norm.generic_string();
        while (out.size() > 1 && out.back() == '/')
            out.pop_back();
        return out;
    }

    static std::string basenameOf(const std::string& path)
    {
        std::size_t pos = path.find_last_of('/');
        if (pos == std::string::npos)
            return path;
        if (pos + 1 >= path.size())
            return {};
        return path.substr(pos + 1);
    }

    static bool pathHasSuffix(const std::string& path, const std::string& suffix)
    {
        if (suffix.empty())
            return false;
        if (path.size() < suffix.size())
            return false;
        if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
            return false;
        if (path.size() == suffix.size())
            return true;
        return path[path.size() - suffix.size() - 1] == '/';
    }

    static bool pathHasPrefix(const std::string& path, const std::string& prefix)
    {
        if (prefix.empty())
            return false;
        if (path.size() < prefix.size())
            return false;
        if (path.compare(0, prefix.size(), prefix) != 0)
            return false;
        if (path.size() == prefix.size())
            return true;
        return path[prefix.size()] == '/';
    }

    bool shouldIncludePath(const std::string& path, const AnalysisConfig& config)
    {
        if (config.onlyFiles.empty() && config.onlyDirs.empty())
            return true;
        if (path.empty())
            return false;

        const std::string normPath = normalizePathForMatch(path);

        for (const auto& file : config.onlyFiles)
        {
            const std::string normFile = normalizePathForMatch(file);
            if (normPath == normFile || pathHasSuffix(normPath, normFile))
                return true;
            const std::string fileBase = basenameOf(normFile);
            if (!fileBase.empty() && basenameOf(normPath) == fileBase)
                return true;
        }

        for (const auto& dir : config.onlyDirs)
        {
            const std::string normDir = normalizePathForMatch(dir);
            if (pathHasPrefix(normPath, normDir) || pathHasSuffix(normPath, normDir))
                return true;
            const std::string needle = "/" + normDir + "/";
            if (normPath.find(needle) != std::string::npos)
                return true;
        }

        return false;
    }

    bool functionNameMatches(const llvm::Function& F, const AnalysisConfig& config)
    {
        if (config.onlyFunctions.empty())
            return true;

        auto itaniumBaseName = [](const std::string& symbol) -> std::string
        {
            if (symbol.rfind("_Z", 0) != 0)
                return {};
            std::size_t i = 2;
            if (i < symbol.size() && symbol[i] == 'L')
                ++i;
            if (i >= symbol.size() || !std::isdigit(static_cast<unsigned char>(symbol[i])))
                return {};
            std::size_t len = 0;
            while (i < symbol.size() && std::isdigit(static_cast<unsigned char>(symbol[i])))
            {
                len = len * 10 + static_cast<std::size_t>(symbol[i] - '0');
                ++i;
            }
            if (len == 0 || i + len > symbol.size())
                return {};
            return symbol.substr(i, len);
        };

        std::string name = F.getName().str();
        std::string demangledName;
        if (ctrace_tools::isMangled(name) || name.rfind("_Z", 0) == 0)
            demangledName = ctrace_tools::demangle(name.c_str());
        std::string demangledBase;
        if (!demangledName.empty())
        {
            std::size_t pos = demangledName.find('(');
            if (pos != std::string::npos && pos > 0)
                demangledBase = demangledName.substr(0, pos);
        }
        std::string itaniumBase = itaniumBaseName(name);

        for (const auto& filter : config.onlyFunctions)
        {
            if (name == filter)
                return true;
            if (!demangledName.empty() && demangledName == filter)
                return true;
            if (!demangledBase.empty() && demangledBase == filter)
                return true;
            if (!itaniumBase.empty() && itaniumBase == filter)
                return true;
            if (ctrace_tools::isMangled(filter))
            {
                std::string demangledFilter = ctrace_tools::demangle(filter.c_str());
                if (!demangledName.empty() && demangledName == demangledFilter)
                    return true;
                std::size_t pos = demangledFilter.find('(');
                if (pos != std::string::npos && pos > 0)
                {
                    std::string demangledFilterBase = demangledFilter.substr(0, pos);
                    if (!demangledBase.empty() && demangledBase == demangledFilterBase)
                        return true;
                }
            }
        }

        return false;
    }
} // namespace ctrace::stack::analysis
