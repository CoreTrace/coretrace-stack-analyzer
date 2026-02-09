#include "analysis/FunctionFilter.hpp"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/AnalyzerUtils.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
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

        static std::string toLowerCopy(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            for (char c : input)
            {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return out;
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

        static bool pathContainsFragment(const std::string& path, const std::string& fragment)
        {
            return !fragment.empty() && path.find(fragment) != std::string::npos;
        }

        static bool isLikelySystemPath(const std::string& path)
        {
            if (path.empty())
                return false;

            const std::string normalized = toLowerCopy(normalizePathForMatch(path));
            if (normalized.empty())
                return false;

            static constexpr std::array<const char*, 15> systemPrefixes = {
                "/usr/include",
                "/usr/lib",
                "/usr/local/include",
                "/usr/local/lib",
                "/opt/homebrew/include",
                "/opt/homebrew/lib",
                "/opt/homebrew/cellar",
                "/opt/local/include",
                "/opt/local/lib",
                "/library/developer/commandlinetools/usr/include",
                "/library/developer/commandlinetools/usr/lib",
                "/applications/xcode.app/contents/developer/toolchains",
                "/applications/xcode.app/contents/developer/platforms",
                "/nix/store",
                "c:/program files"};

            for (const char* prefix : systemPrefixes)
            {
                if (pathHasPrefix(normalized, prefix))
                    return true;
            }

            static constexpr std::array<const char*, 5> systemFragments = {
                "/include/c++/", "/c++/v1/", "/lib/clang/", "/x86_64-linux-gnu/c++/",
                "/aarch64-linux-gnu/c++/"};

            for (const char* fragment : systemFragments)
            {
                if (pathContainsFragment(normalized, fragment))
                    return true;
            }

            return false;
        }

        static bool isCompilerRuntimeLikeName(llvm::StringRef name)
        {
            return name.starts_with("llvm.") || name.starts_with("clang.") ||
                   name.starts_with("__asan_") || name.starts_with("__ubsan_") ||
                   name.starts_with("__tsan_") || name.starts_with("__msan_");
        }
    } // namespace

    FunctionFilter buildFunctionFilter(const llvm::Module& mod, const AnalysisConfig& config)
    {
        FunctionFilter filter;
        filter.hasPathFilter = !config.onlyFiles.empty() || !config.onlyDirs.empty();
        filter.hasFuncFilter = !config.onlyFunctions.empty();
        filter.hasFilter = filter.hasPathFilter || filter.hasFuncFilter || !config.includeSTL;
        filter.moduleSourcePath = mod.getSourceFileName();
        filter.config = &config;
        return filter;
    }

    bool FunctionFilter::shouldAnalyze(const llvm::Function& F) const
    {
        if (!config)
            return true;

        const AnalysisConfig& cfg = *config;

        if (!hasFilter)
            return true;
        if (hasFuncFilter && !functionNameMatches(F, cfg))
        {
            if (cfg.dumpFilter)
            {
                llvm::errs() << "[filter] func=" << F.getName() << " file=<name-filter> keep=no\n";
            }
            return false;
        }
        if (!hasPathFilter)
        {
            if (!cfg.includeSTL && !hasFuncFilter)
            {
                std::string path = getFunctionSourcePath(F);
                std::string usedPath;
                bool decision = true;

                if (!path.empty())
                {
                    usedPath = path;
                    decision = !isLikelySystemPath(usedPath);
                }
                else if (!moduleSourcePath.empty())
                {
                    usedPath = moduleSourcePath;
                    decision = !isLikelySystemPath(usedPath);
                }
                else
                {
                    decision = !isCompilerRuntimeLikeName(F.getName());
                }

                if (cfg.dumpFilter)
                {
                    llvm::errs() << "[filter] func=" << F.getName() << " file=";
                    if (usedPath.empty())
                        llvm::errs() << "<none>";
                    else
                        llvm::errs() << usedPath;
                    llvm::errs() << " keep=" << (decision ? "yes" : "no") << "\n";
                }
                return decision;
            }
            return true;
        }
        std::string path = getFunctionSourcePath(F);
        std::string usedPath;
        bool decision = false;
        if (!path.empty())
        {
            usedPath = path;
            decision = shouldIncludePath(usedPath, cfg);
        }
        else
        {
            llvm::StringRef name = F.getName();
            if (name.starts_with("__") || name.starts_with("llvm.") || name.starts_with("clang."))
            {
                decision = false;
            }
            else if (!moduleSourcePath.empty())
            {
                usedPath = moduleSourcePath;
                decision = shouldIncludePath(usedPath, cfg);
            }
            else
            {
                decision = false;
            }
        }

        if (cfg.dumpFilter)
        {
            llvm::errs() << "[filter] func=" << F.getName() << " file=";
            if (usedPath.empty())
                llvm::errs() << "<none>";
            else
                llvm::errs() << usedPath;
            llvm::errs() << " keep=" << (decision ? "yes" : "no") << "\n";
        }

        return decision;
    }
} // namespace ctrace::stack::analysis
