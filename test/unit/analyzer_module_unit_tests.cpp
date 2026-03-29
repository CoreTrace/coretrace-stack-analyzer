// SPDX-License-Identifier: Apache-2.0
#include "StackUsageAnalyzer.hpp"
#include "analysis/InputPipeline.hpp"
#include "analysis/Reachability.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analyzer/LocationResolver.hpp"
#include "analyzer/ModulePreparationService.hpp"

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

namespace
{
    struct LoadedModule
    {
        llvm::LLVMContext context;
        std::unique_ptr<llvm::Module> module;
    };

    struct TestReport
    {
        int failures = 0;

        void expect(bool condition, const std::string& message)
        {
            if (!condition)
            {
                ++failures;
                std::cerr << "[FAIL] " << message << "\n";
            }
            else
            {
                std::cout << "[PASS] " << message << "\n";
            }
        }
    };

    bool loadModuleFromSource(const std::filesystem::path& sourceFile,
                              const ctrace::stack::AnalysisConfig& config, LoadedModule& out,
                              std::string& errorOut)
    {
        llvm::SMDiagnostic err;
        ctrace::stack::analysis::ModuleLoadResult load =
            ctrace::stack::analysis::loadModuleForAnalysis(sourceFile.string(), config, out.context,
                                                           err);
        if (!load.module)
        {
            errorOut = load.error;
            if (err.getLineNo() != 0 || !err.getFilename().empty())
            {
                std::string diagText;
                llvm::raw_string_ostream os(diagText);
                err.print("stack_usage_analyzer_unit_tests", os);
                os.flush();
                errorOut += diagText;
            }
            return false;
        }

        out.module = std::move(load.module);
        return true;
    }

    bool testLocationResolver(const std::filesystem::path& repoRoot, TestReport& report)
    {
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/alloca/oversized-constant.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "LocationResolver setup: failed to load module: " + loadError);
            return false;
        }

        const llvm::Instruction* instructionWithDebug = nullptr;
        const llvm::AllocaInst* firstAlloca = nullptr;

        for (llvm::Function& F : *loaded.module)
        {
            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {
                    if (instructionWithDebug == nullptr && I.getDebugLoc())
                        instructionWithDebug = &I;
                    if (firstAlloca == nullptr)
                        firstAlloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
                }
            }
        }

        const ctrace::stack::analyzer::ResolvedLocation nullLoc =
            ctrace::stack::analyzer::resolveFromInstruction(nullptr, true);
        report.expect(!nullLoc.hasLocation,
                      "LocationResolver: null instruction returns no location");

        report.expect(instructionWithDebug != nullptr,
                      "LocationResolver: found an instruction with debug info");
        if (instructionWithDebug != nullptr)
        {
            const ctrace::stack::analyzer::ResolvedLocation loc =
                ctrace::stack::analyzer::resolveFromInstruction(instructionWithDebug, true);
            report.expect(loc.hasLocation, "LocationResolver: resolveFromInstruction has location");
            report.expect(loc.line > 0, "LocationResolver: resolved line > 0");
            report.expect(loc.column > 0, "LocationResolver: resolved column > 0");
            report.expect(loc.startLine == loc.line,
                          "LocationResolver: startLine matches line for single instruction");
            report.expect(loc.endLine == loc.line,
                          "LocationResolver: endLine matches line for single instruction");
        }

        report.expect(firstAlloca != nullptr, "LocationResolver: found alloca instruction");
        if (firstAlloca != nullptr)
        {
            unsigned line = 0;
            unsigned column = 0;
            const bool ok =
                ctrace::stack::analyzer::resolveAllocaSourceLocation(firstAlloca, line, column);
            report.expect(ok, "LocationResolver: resolveAllocaSourceLocation succeeded");
            report.expect(line > 0, "LocationResolver: alloca source line > 0");
            report.expect(column > 0, "LocationResolver: alloca source column > 0");
        }

        return true;
    }

    bool testReachabilityService(const std::filesystem::path& repoRoot, TestReport& report)
    {
        const ctrace::stack::AnalysisConfig config;

        auto verifyFixture = [&](const std::filesystem::path& sourcePath, bool expectUnreachable,
                                 const std::string& fixtureLabel)
        {
            LoadedModule loaded;
            std::string loadError;
            if (!loadModuleFromSource(sourcePath, config, loaded, loadError))
            {
                report.expect(false, "Reachability setup: failed to load module: " + loadError);
                return;
            }

            std::function<bool(const llvm::Function&)> shouldAnalyze = [](const llvm::Function&)
            { return true; };
            const auto issues = ctrace::stack::analysis::analyzeStackBufferOverflows(
                *loaded.module, shouldAnalyze, config);
            report.expect(!issues.empty(), fixtureLabel + " produced at least one buffer issue");

            bool foundExpectedClassification = false;
            for (const auto& issue : issues)
            {
                const bool isUnreachable =
                    ctrace::stack::analysis::isStaticallyUnreachableStackAccess(issue);
                if (isUnreachable == expectUnreachable)
                {
                    foundExpectedClassification = true;
                    break;
                }
            }

            if (expectUnreachable)
            {
                report.expect(foundExpectedClassification,
                              fixtureLabel +
                                  " detects statically unreachable stack access in fixture");
            }
            else
            {
                report.expect(foundExpectedClassification,
                              fixtureLabel + " keeps non-unreachable stack accesses as reachable");
            }
        };

        verifyFixture(repoRoot / "test/bound-storage/unreachable-validation.c", true,
                      "Reachability: unreachable fixture");
        verifyFixture(repoRoot / "test/bound-storage/bound-storage.c", false,
                      "Reachability: baseline fixture");

        return true;
    }

    bool testModulePreparationService(const std::filesystem::path& repoRoot, TestReport& report)
    {
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/no-error/basic-main.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false,
                          "ModulePreparationService setup: failed to load module: " + loadError);
            return false;
        }

        ctrace::stack::analyzer::ModulePreparationService service;
        ctrace::stack::analyzer::PreparedModule prepared = service.prepare(*loaded.module, config);

        report.expect(!prepared.ctx.allDefinedFunctions.empty(),
                      "ModulePreparationService: has defined functions");
        report.expect(!prepared.ctx.functions.empty(),
                      "ModulePreparationService: has analyzable functions");
        report.expect(prepared.localStack.size() == prepared.ctx.allDefinedFunctions.size(),
                      "ModulePreparationService: localStack covers all defined functions");

        bool graphCoversAll = true;
        for (llvm::Function* F : prepared.ctx.allDefinedFunctions)
        {
            if (prepared.callGraph.find(F) == prepared.callGraph.end())
            {
                graphCoversAll = false;
                break;
            }
        }
        report.expect(graphCoversAll, "ModulePreparationService: call graph covers all functions");

        const llvm::Function* mainFn = loaded.module->getFunction("main");
        report.expect(mainFn != nullptr, "ModulePreparationService: main function exists");
        if (mainFn != nullptr)
        {
            report.expect(prepared.ctx.isDefined(*mainFn),
                          "ModulePreparationService: main is in defined set");
            report.expect(prepared.ctx.shouldAnalyze(*mainFn),
                          "ModulePreparationService: main is analyzable");
        }

        report.expect(prepared.recursionState.InfiniteRecursionFuncs.empty(),
                      "ModulePreparationService: baseline fixture has no infinite recursion");

        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: stack_usage_analyzer_unit_tests <repo-root>\n";
        return 2;
    }

    const std::filesystem::path repoRoot = std::filesystem::path(argv[1]);
    TestReport report;

    (void)testLocationResolver(repoRoot, report);
    (void)testReachabilityService(repoRoot, report);
    (void)testModulePreparationService(repoRoot, report);

    if (report.failures == 0)
    {
        std::cout << "All analyzer module unit tests passed.\n";
        return 0;
    }

    std::cerr << report.failures << " analyzer module unit test(s) failed.\n";
    return 1;
}
