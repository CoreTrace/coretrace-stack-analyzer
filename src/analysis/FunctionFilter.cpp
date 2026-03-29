// SPDX-License-Identifier: Apache-2.0
#include "analysis/FunctionFilter.hpp"

#include <array>
#include <cctype>
#include <filesystem>
#include <string>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <coretrace/logger.hpp>

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
            std::filesystem::path norm = path.lexically_normal();
            if (norm.is_absolute())
            {
                std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(norm, ec);
                if (!ec)
                    norm = canonicalPath;
            }
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

        static bool pathHasSuffix(const std::string& path, const std::string& suffix)
        {
            if (suffix.empty() || path.size() < suffix.size())
                return false;
            return path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        static bool pathContainsSegment(const std::string& path, const std::string& segment)
        {
            if (segment.empty() || path.empty())
                return false;

            std::size_t start = 0;
            while (start < path.size())
            {
                while (start < path.size() && path[start] == '/')
                    ++start;
                if (start >= path.size())
                    break;
                std::size_t end = path.find('/', start);
                if (end == std::string::npos)
                    end = path.size();
                if (path.compare(start, end - start, segment) == 0)
                    return true;
                start = end + 1;
            }
            return false;
        }

        static std::string pathBasename(const std::string& path)
        {
            if (path.empty())
                return {};
            const std::size_t slash = path.find_last_of('/');
            if (slash == std::string::npos)
                return path;
            if (slash + 1 >= path.size())
                return {};
            return path.substr(slash + 1);
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

        static bool isLikelyThirdPartyPath(const std::string& path)
        {
            if (path.empty())
                return false;

            const std::string normalized = toLowerCopy(normalizePathForMatch(path));
            if (normalized.empty())
                return false;

            static constexpr std::array<const char*, 13> thirdPartySegments = {
                "third_party", "third-party",     "thirdparty", "3rdparty", "vendor",
                "vendors",     "external",        "extern",     "deps",     "_deps",
                "submodules",  "vcpkg_installed", "conan"};
            for (const char* segment : thirdPartySegments)
            {
                if (pathContainsSegment(normalized, segment))
                    return true;
            }

            const std::string base = pathBasename(normalized);
            if (base.rfind("stb_", 0) == 0 &&
                (pathHasSuffix(base, ".h") || pathHasSuffix(base, ".hh") ||
                 pathHasSuffix(base, ".hpp") || pathHasSuffix(base, ".hxx")))
            {
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

        static void logFilterDecision(const llvm::Function& F, const std::string& file, bool keep)
        {
            coretrace::log(coretrace::Level::Info, "[filter] func={} file={} keep={}\n",
                           F.getName().str(), file, keep ? "yes" : "no");
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
                logFilterDecision(F, "<name-filter>", false);
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
                    decision = !isLikelySystemPath(usedPath) && !isLikelyThirdPartyPath(usedPath);
                }
                else if (!moduleSourcePath.empty())
                {
                    usedPath = moduleSourcePath;
                    decision = !isLikelySystemPath(usedPath) && !isLikelyThirdPartyPath(usedPath);
                }
                else
                {
                    decision = !isCompilerRuntimeLikeName(F.getName());
                }

                if (cfg.dumpFilter)
                {
                    logFilterDecision(F, usedPath.empty() ? std::string("<none>") : usedPath,
                                      decision);
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
            logFilterDecision(F, usedPath.empty() ? std::string("<none>") : usedPath, decision);
        }

        return decision;
    }
} // namespace ctrace::stack::analysis
