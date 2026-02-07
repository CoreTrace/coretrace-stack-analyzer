#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ctrace::stack::analysis
{
    struct CompileCommand
    {
        std::string directory;
        std::vector<std::string> arguments;
    };

    class CompilationDatabase
    {
      public:
        static std::shared_ptr<CompilationDatabase> loadFromFile(const std::string& path,
                                                                 std::string& error);

        const CompileCommand* findCommandForFile(const std::string& filePath) const;

        const std::string& sourcePath() const
        {
            return sourcePath_;
        }

      private:
        std::string sourcePath_;
        std::unordered_map<std::string, CompileCommand> commands_;
    };
} // namespace ctrace::stack::analysis
