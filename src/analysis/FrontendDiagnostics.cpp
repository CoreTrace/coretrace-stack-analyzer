#include "analysis/FrontendDiagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Module.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        enum class ParsedSeverity
        {
            Warning,
            Error
        };

        struct ParsedFrontendWarning
        {
            std::string filePath;
            unsigned line = 0;
            unsigned column = 0;
            ParsedSeverity severity = ParsedSeverity::Warning;
            std::string message;
        };

        struct Classification
        {
            std::string ruleId;
            std::string cwe;
            std::string summary;
        };

        static std::string trim(std::string s)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
            s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
            return s;
        }

        static std::string toLower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        static std::string normalizePath(std::string path)
        {
            path = trim(std::move(path));
            if (path.empty())
                return path;

            for (char& c : path)
            {
                if (c == '\\')
                    c = '/';
            }

            std::error_code ec;
            std::filesystem::path abs = std::filesystem::absolute(path, ec);
            if (ec)
                abs = std::filesystem::path(path);

            std::filesystem::path canon = std::filesystem::weakly_canonical(abs, ec);
            std::filesystem::path out = ec ? abs.lexically_normal() : canon;
            return out.generic_string();
        }

        static std::string basenameOf(std::string path)
        {
            for (char& c : path)
            {
                if (c == '\\')
                    c = '/';
            }
            std::size_t slash = path.find_last_of('/');
            if (slash == std::string::npos)
                return path;
            if (slash + 1 >= path.size())
                return {};
            return path.substr(slash + 1);
        }

        static bool parseUnsigned(const std::string& text, unsigned& out)
        {
            if (text.empty())
                return false;
            std::uint64_t value = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                    return false;
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
                if (value > std::numeric_limits<unsigned>::max())
                    return false;
            }
            out = static_cast<unsigned>(value);
            return true;
        }

        static bool parseFrontendWarningLine(const std::string& line, ParsedFrontendWarning& out)
        {
            std::size_t markerPos = line.find(": warning: ");
            ParsedSeverity severity = ParsedSeverity::Warning;
            std::size_t markerLen = std::string(": warning: ").size();
            if (markerPos == std::string::npos)
            {
                markerPos = line.find(": error: ");
                if (markerPos == std::string::npos)
                    return false;
                markerLen = std::string(": error: ").size();
                severity = ParsedSeverity::Error;
            }

            const std::string prefix = line.substr(0, markerPos);
            const std::string message = trim(line.substr(markerPos + markerLen));
            if (message.empty())
                return false;

            const std::size_t colSep = prefix.rfind(':');
            if (colSep == std::string::npos)
                return false;
            const std::size_t lineSep = prefix.rfind(':', colSep - 1);
            if (lineSep == std::string::npos)
                return false;

            unsigned parsedLine = 0;
            unsigned parsedColumn = 0;
            const std::string lineStr = prefix.substr(lineSep + 1, colSep - lineSep - 1);
            const std::string colStr = prefix.substr(colSep + 1);
            if (!parseUnsigned(lineStr, parsedLine) || !parseUnsigned(colStr, parsedColumn))
                return false;

            std::string filePath = trim(prefix.substr(0, lineSep));
            const std::size_t trailingSpace = filePath.find_last_of(" \t");
            if (trailingSpace != std::string::npos)
                filePath = filePath.substr(trailingSpace + 1);
            filePath = trim(std::move(filePath));
            if (filePath.empty())
                return false;

            out.filePath = filePath;
            out.line = parsedLine;
            out.column = parsedColumn;
            out.severity = severity;
            out.message = message;
            return true;
        }

        static std::optional<Classification> classifyMessage(const std::string& message)
        {
            const std::string m = toLower(message);

            if (m.find("format string is not a string literal") != std::string::npos)
            {
                return Classification{"FormatString.NonLiteral", "CWE-134",
                                      "non-literal format string may allow format injection"};
            }

            if (m.find("format specifies type") != std::string::npos ||
                m.find("more '%' conversions than data arguments") != std::string::npos ||
                m.find("data argument not used by format string") != std::string::npos)
            {
                return Classification{"VariadicFormatMismatch", "CWE-685",
                                      "variadic format and argument list appear inconsistent"};
            }

            if (m.find("sizeof on array function parameter") != std::string::npos ||
                m.find("will return the size of the pointer") != std::string::npos ||
                (m.find("call operates on objects of type") != std::string::npos &&
                 m.find("size is based on a different type") != std::string::npos))
            {
                return Classification{"SizeofPitfall", "CWE-467",
                                      "size computation appears to use pointer size instead of "
                                      "object size"};
            }

            if (m.find("'gets' is deprecated") != std::string::npos)
            {
                return Classification{"UnsafeFunction.DeprecatedGets", "CWE-676",
                                      "deprecated unsafe function 'gets' is used"};
            }

            return std::nullopt;
        }

        static std::string combineDebugFilePath(const llvm::DIFile* file)
        {
            if (!file)
                return {};
            std::string directory = file->getDirectory().str();
            std::string filename = file->getFilename().str();
            if (filename.empty())
                return {};
            if (directory.empty())
                return filename;
            return directory + "/" + filename;
        }

        static std::string resolveLocationFile(const llvm::DebugLoc& loc)
        {
            if (!loc)
                return {};
            const llvm::DILocalScope* scope = loc->getScope();
            if (!scope)
                return {};
            if (const llvm::DIFile* file = scope->getFile())
                return combineDebugFilePath(file);
            if (const llvm::DISubprogram* sp = scope->getSubprogram())
                return combineDebugFilePath(sp->getFile());
            return {};
        }

        static bool filePathsLikelyMatch(const std::string& lhsPath, const std::string& rhsPath)
        {
            if (lhsPath.empty() || rhsPath.empty())
                return false;

            const std::string lhsNorm = normalizePath(lhsPath);
            const std::string rhsNorm = normalizePath(rhsPath);
            if (!lhsNorm.empty() && lhsNorm == rhsNorm)
                return true;

            const std::string lhsBase = basenameOf(lhsPath);
            const std::string rhsBase = basenameOf(rhsPath);
            return !lhsBase.empty() && lhsBase == rhsBase;
        }

        static std::string resolveFunctionNameForLocation(const llvm::Module& mod,
                                                          const std::string& filePath,
                                                          unsigned line)
        {
            const llvm::Function* best = nullptr;
            unsigned bestDistance = std::numeric_limits<unsigned>::max();

            for (const llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;

                bool sawCandidateFile = false;
                unsigned localBestDistance = std::numeric_limits<unsigned>::max();
                for (const llvm::BasicBlock& BB : F)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        const llvm::DebugLoc dl = I.getDebugLoc();
                        if (!dl)
                            continue;

                        const std::string debugFile = resolveLocationFile(dl);
                        if (!filePathsLikelyMatch(debugFile, filePath))
                            continue;

                        sawCandidateFile = true;
                        const unsigned debugLine = dl.getLine();
                        if (debugLine == 0)
                            continue;
                        if (debugLine == line)
                            return F.getName().str();

                        const unsigned distance =
                            (debugLine > line) ? (debugLine - line) : (line - debugLine);
                        localBestDistance = std::min(localBestDistance, distance);
                    }
                }

                if (sawCandidateFile && localBestDistance < bestDistance)
                {
                    best = &F;
                    bestDistance = localBestDistance;
                }
            }

            return best ? best->getName().str() : std::string{};
        }

        static std::string severityPrefix(DiagnosticSeverity severity)
        {
            switch (severity)
            {
            case DiagnosticSeverity::Info:
                return "[ !Info! ]";
            case DiagnosticSeverity::Warning:
                return "[ !!Warn ]";
            case DiagnosticSeverity::Error:
                return "[!!!Error]";
            }
            return "[ !!Warn ]";
        }
    } // namespace

    std::vector<Diagnostic> collectFrontendDiagnostics(const std::string& diagnosticsText,
                                                       const llvm::Module& mod,
                                                       const std::string& fallbackFilePath)
    {
        std::vector<Diagnostic> out;
        if (diagnosticsText.empty())
            return out;

        std::unordered_set<std::string> seen;
        std::istringstream stream(diagnosticsText);
        std::string line;
        while (std::getline(stream, line))
        {
            ParsedFrontendWarning parsed;
            if (!parseFrontendWarningLine(line, parsed))
                continue;

            const std::optional<Classification> classification = classifyMessage(parsed.message);
            if (!classification)
                continue;

            const std::string key =
                normalizePath(parsed.filePath) + ":" + std::to_string(parsed.line) + ":" +
                std::to_string(parsed.column) + ":" + classification->ruleId + ":" + parsed.message;
            if (!seen.insert(key).second)
                continue;

            Diagnostic diag;
            diag.filePath = parsed.filePath.empty() ? fallbackFilePath : parsed.filePath;
            if (diag.filePath.empty())
                diag.filePath = fallbackFilePath;
            diag.funcName = resolveFunctionNameForLocation(mod, diag.filePath, parsed.line);
            diag.line = parsed.line;
            diag.column = parsed.column;
            diag.startLine = parsed.line;
            diag.startColumn = parsed.column;
            diag.endLine = parsed.line;
            diag.endColumn = parsed.column;
            diag.severity = (parsed.severity == ParsedSeverity::Error)
                                ? DiagnosticSeverity::Error
                                : DiagnosticSeverity::Warning;
            diag.errCode = DescriptiveErrorCode::None;
            diag.ruleId = classification->ruleId;
            diag.cweId = classification->cwe;
            diag.confidence = 0.85;

            std::ostringstream msg;
            msg << "\t" << severityPrefix(diag.severity) << " " << classification->summary << "\n"
                << "\t\t ↳ clang: " << parsed.message << "\n";
            diag.message = msg.str();

            out.push_back(std::move(diag));
        }

        return out;
    }
} // namespace ctrace::stack::analysis
