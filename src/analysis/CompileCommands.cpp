#include "analysis/CompileCommands.hpp"

#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        std::string normalizePath(const std::filesystem::path& path)
        {
            if (path.empty())
                return {};

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

        bool pathHasSuffix(const std::string& path, const std::string& suffix)
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

        std::vector<std::string> buildPathSuffixes(const std::string& path)
        {
            std::vector<std::string> suffixes;
            if (path.empty())
                return suffixes;

            suffixes.push_back(path);
            for (std::size_t i = 1; i < path.size(); ++i)
            {
                if (path[i] == '/' && i + 1 < path.size())
                    suffixes.push_back(path.substr(i));
            }
            return suffixes;
        }

        std::vector<std::string> tokenizeCommandLine(const std::string& command)
        {
            std::vector<std::string> tokens;
            std::string current;
            enum class State
            {
                Normal,
                SingleQuote,
                DoubleQuote
            };

            State state = State::Normal;
            for (std::size_t i = 0; i < command.size(); ++i)
            {
                char c = command[i];
                if (state == State::Normal)
                {
                    if (std::isspace(static_cast<unsigned char>(c)))
                    {
                        if (!current.empty())
                        {
                            tokens.push_back(current);
                            current.clear();
                        }
                        continue;
                    }
                    if (c == '\'')
                    {
                        state = State::SingleQuote;
                        continue;
                    }
                    if (c == '"')
                    {
                        state = State::DoubleQuote;
                        continue;
                    }
                    if (c == '\\' && i + 1 < command.size())
                    {
                        current.push_back(command[++i]);
                        continue;
                    }
                    current.push_back(c);
                    continue;
                }

                if (state == State::SingleQuote)
                {
                    if (c == '\'')
                    {
                        state = State::Normal;
                        continue;
                    }
                    current.push_back(c);
                    continue;
                }

                if (c == '"')
                {
                    state = State::Normal;
                    continue;
                }
                if (c == '\\' && i + 1 < command.size())
                {
                    current.push_back(command[++i]);
                    continue;
                }
                current.push_back(c);
            }

            if (!current.empty())
                tokens.push_back(current);

            return tokens;
        }

        void stripOutputAndDependencyArgs(std::vector<std::string>& args)
        {
            std::vector<std::string> filtered;
            filtered.reserve(args.size());

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string& arg = args[i];
                if (arg == "-o" || arg == "--output")
                {
                    if (i + 1 < args.size())
                        ++i;
                    continue;
                }
                if (arg.size() > 2 && arg.rfind("-o", 0) == 0)
                    continue;

                if (arg == "-MF" || arg == "-MT" || arg == "-MQ")
                {
                    if (i + 1 < args.size())
                        ++i;
                    continue;
                }
                if ((arg.size() > 3 && (arg.rfind("-MF", 0) == 0 || arg.rfind("-MT", 0) == 0 ||
                                        arg.rfind("-MQ", 0) == 0)))
                    continue;

                if (arg == "-M" || arg == "-MM" || arg == "-MD" || arg == "-MMD" || arg == "-MG" ||
                    arg == "-MP")
                    continue;

                filtered.push_back(arg);
            }

            args.swap(filtered);
        }

        void stripInputFileArg(std::vector<std::string>& args, const std::string& directory,
                               const std::string& fileKey)
        {
            if (fileKey.empty())
                return;

            std::vector<std::string> filtered;
            filtered.reserve(args.size());
            bool removed = false;

            for (const auto& arg : args)
            {
                if (!removed && !arg.empty() && arg[0] != '-')
                {
                    std::filesystem::path argPath(arg);
                    if (argPath.is_relative())
                        argPath = std::filesystem::path(directory) / argPath;
                    std::string argKey = normalizePath(argPath);
                    if (!argKey.empty() && argKey == fileKey)
                    {
                        removed = true;
                        continue;
                    }
                }

                filtered.push_back(arg);
            }

            args.swap(filtered);
        }

        std::vector<std::string> extractArguments(const llvm::json::Object& obj)
        {
            std::vector<std::string> args;
            if (auto* arr = obj.getArray("arguments"))
            {
                args.reserve(arr->size());
                for (const auto& value : *arr)
                {
                    if (auto str = value.getAsString())
                        args.push_back(str->str());
                }
                return args;
            }

            if (auto command = obj.getString("command"))
                return tokenizeCommandLine(command->str());

            return args;
        }

        void stripLeadingCommandTokens(std::vector<std::string>& args)
        {
            std::size_t start = 0;
            while (start < args.size())
            {
                const std::string& token = args[start];
                if (!token.empty() && (token[0] == '-' || token[0] == '@'))
                    break;
                ++start;
            }
            if (start > 0)
                args.erase(args.begin(), args.begin() + static_cast<std::ptrdiff_t>(start));
        }

        std::filesystem::path normalizeDirectoryPath(const std::filesystem::path& compdbDir,
                                                     const std::string& directory)
        {
            std::filesystem::path dirPath =
                directory.empty() ? compdbDir : std::filesystem::path(directory);
            if (dirPath.is_relative())
                dirPath = compdbDir / dirPath;
            return dirPath;
        }
    } // namespace

    std::shared_ptr<CompilationDatabase> CompilationDatabase::loadFromFile(const std::string& path,
                                                                           std::string& error)
    {
        error.clear();
        auto bufferOrErr = llvm::MemoryBuffer::getFile(path);
        if (!bufferOrErr)
        {
            error = "unable to read compile commands file: " + path + " (" +
                    bufferOrErr.getError().message() + ")";
            return nullptr;
        }

        auto parsed = llvm::json::parse(bufferOrErr.get()->getBuffer());
        if (!parsed)
        {
            error = "failed to parse compile commands JSON: " + llvm::toString(parsed.takeError());
            return nullptr;
        }

        auto* array = parsed->getAsArray();
        if (!array)
        {
            error = "compile commands JSON must be an array";
            return nullptr;
        }

        auto db = std::make_shared<CompilationDatabase>();
        db->sourcePath_ = normalizePath(path);

        std::filesystem::path compdbDir = std::filesystem::path(path).parent_path();
        if (compdbDir.empty())
        {
            std::error_code ec;
            compdbDir = std::filesystem::current_path(ec);
            if (ec)
                compdbDir = std::filesystem::path(".");
        }

        for (const auto& entryValue : *array)
        {
            const auto* obj = entryValue.getAsObject();
            if (!obj)
                continue;

            auto fileValue = obj->getString("file");
            if (!fileValue)
                continue;

            std::string fileStr = fileValue->str();
            std::string dirStr;
            if (auto directoryValue = obj->getString("directory"))
                dirStr = directoryValue->str();

            std::filesystem::path directoryPath = normalizeDirectoryPath(compdbDir, dirStr);
            std::string directoryKey = normalizePath(directoryPath);
            if (directoryKey.empty())
                continue;

            std::filesystem::path filePath(fileStr);
            if (filePath.is_relative())
                filePath = directoryPath / filePath;
            std::string fileKey = normalizePath(filePath);
            if (fileKey.empty())
                continue;

            std::vector<std::string> args = extractArguments(*obj);
            if (args.empty())
                continue;

            stripLeadingCommandTokens(args);
            stripOutputAndDependencyArgs(args);
            stripInputFileArg(args, directoryKey, fileKey);

            if (db->commands_.find(fileKey) != db->commands_.end())
                continue;

            CompileCommand command;
            command.directory = directoryKey;
            command.arguments = std::move(args);
            db->commands_.emplace(fileKey, std::move(command));
        }

        if (db->commands_.empty())
        {
            error = "compile commands file contains no usable entries";
            return nullptr;
        }

        return db;
    }

    const CompileCommand* CompilationDatabase::findCommandForFile(const std::string& filePath) const
    {
        if (filePath.empty())
            return nullptr;
        std::string key = normalizePath(std::filesystem::path(filePath));
        auto it = commands_.find(key);
        if (it == commands_.end())
        {
            auto suffixes = buildPathSuffixes(key);
            if (!suffixes.empty())
            {
                // Skip the full path; we already attempted exact lookup.
                for (std::size_t s = 1; s < suffixes.size(); ++s)
                {
                    const std::string& suffix = suffixes[s];
                    const CompileCommand* match = nullptr;
                    std::size_t matchCount = 0;
                    for (const auto& entry : commands_)
                    {
                        if (pathHasSuffix(entry.first, suffix))
                        {
                            ++matchCount;
                            if (matchCount == 1)
                                match = &entry.second;
                            else
                                break;
                        }
                    }
                    if (matchCount == 1)
                        return match;
                    if (matchCount > 1)
                        break;
                }
            }
            return nullptr;
        }
        return &it->second;
    }
} // namespace ctrace::stack::analysis
