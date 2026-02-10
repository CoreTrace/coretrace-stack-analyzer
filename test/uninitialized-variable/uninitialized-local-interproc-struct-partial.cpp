#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <iostream>

using StackSize = std::uint64_t;

enum class AnalysisMode
{
    IR,
    ABI
};

// Analysis configuration (mode + stack limit).
struct AnalysisConfig
{
    AnalysisMode mode = AnalysisMode::IR;
    StackSize stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB default
    bool quiet = false;
    bool warningsOnly = false;
    std::vector<std::string> extraCompileArgs;
    // std::shared_ptr<const analysis::CompilationDatabase> compilationDatabase;
    bool requireCompilationDatabase = false;
    bool compdbFast = false;
    bool timing = false;
    std::vector<std::string> onlyFiles;
    std::vector<std::string> onlyDirs;
    std::vector<std::string> onlyFunctions;
    bool includeSTL = false;
    bool dumpFilter = false;
    std::string dumpIRPath;
    bool dumpIRIsDir = false;
    int paddedd;
};

int main(void)
{
    AnalysisConfig cfg; // mode = IR, stackLimit = 8 MiB default
    cfg.quiet = false;
    cfg.warningsOnly = false;

    return cfg.paddedd;
}

// at line 43, column 16
// [!!] potential read of uninitialized local variable 'cfg'
//      this load may execute before any definite initialization on all control-flow paths
