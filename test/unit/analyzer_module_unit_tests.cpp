// SPDX-License-Identifier: Apache-2.0
#include "StackUsageAnalyzer.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/ConstSuggestionCheck.hpp"
#include "analysis/ConstSuggestionInternal.hpp"
#include "app/CrossTUSummaryDriver.hpp"
#include "app/AnalyzerApp.hpp"
#include "cli/ArgParser.hpp"
#include "analysis/DuplicateIfCondition.hpp"
#include "analysis/FunctionFacts.hpp"
#include "analysis/InputPipeline.hpp"
#include "analysis/InputPipelineRevision.hpp"
#include "analysis/IntRanges.hpp"
#include "analysis/ParameterDebugBinding.hpp"
#include "analysis/Reachability.hpp"
#include "analysis/ResourceLifetimeAnalysis.hpp"
#include "analysis/ResourceModel.hpp"
#include "analysis/ownership/ResourceFactCollector.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analysis/UninitializedVarAnalysis.hpp"
#include "analysis/smt/SmtEncoding.hpp"
#include "analysis/smt/SmtRefinement.hpp"
#include "analyzer/DiagnosticEmitter.hpp"
#include "analyzer/LocationResolver.hpp"
#include "analyzer/ModulePreparationService.hpp"
#include "mangle.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <llvm/Demangle/Demangle.h>
#include <compilerlib/toolchain.hpp>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <llvm/Analysis/MemorySSA.h>
#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <fcntl.h>
#include <unistd.h>

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

    /// The IntRange map published by computeIntRanges() is consumed by five analyses but is
    /// itself invisible from any diagnostic, so its invariants are pinned here rather than
    /// through a fixture. Notably the multiply-assigned-slot case below cannot be reached by
    /// any detector: they all give up on a rewritten slot before consulting the map, so an
    /// unsound bridge would produce no observable failure end to end.
    bool testIntRangeFacts(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;

        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/int_range_facts_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "IntRangeFacts setup: failed to load module: " + loadError);
            return false;
        }

        llvm::Function* function = loaded.module->getFunction("int_range_facts_input");
        report.expect(function != nullptr, "IntRangeFacts: found int_range_facts_input");
        if (function == nullptr)
            return false;

        const FunctionFacts facts(*function);
        const std::map<const llvm::Value*, IntRange> ranges = computeIntRanges(*function, facts);

        const auto rangeOf = [&ranges](const llvm::Value* value) -> const IntRange*
        {
            const auto it = ranges.find(value);
            return it == ranges.end() ? nullptr : &it->second;
        };

        // Locate the two masking operations by their constant, then the slot each is stored
        // into. `and 255` feeds the single-assignment slot, `and 15` the rewritten one.
        const llvm::Value* maskedSlot = nullptr;
        const llvm::Value* rewrittenSlot = nullptr;
        std::vector<const llvm::Instruction*> nswArithmetic;
        std::vector<const llvm::ZExtInst*> widenings;

        for (llvm::BasicBlock& block : *function)
        {
            for (llvm::Instruction& instruction : block)
            {
                if (const auto* zext = llvm::dyn_cast<llvm::ZExtInst>(&instruction))
                {
                    if (zext->getSrcTy()->isIntegerTy(32) && zext->getDestTy()->isIntegerTy(64))
                        widenings.push_back(zext);
                }

                if (const auto* wrapping =
                        llvm::dyn_cast<llvm::OverflowingBinaryOperator>(&instruction))
                {
                    if (wrapping->hasNoSignedWrap() || wrapping->hasNoUnsignedWrap())
                        nswArithmetic.push_back(&instruction);
                }

                const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (store == nullptr)
                    continue;
                const auto* mask = llvm::dyn_cast<llvm::BinaryOperator>(store->getValueOperand());
                if (mask == nullptr || mask->getOpcode() != llvm::Instruction::And)
                    continue;
                const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(mask->getOperand(1));
                if (constant == nullptr)
                    continue;
                if (constant->getZExtValue() == 255)
                    maskedSlot = store->getPointerOperand();
                else if (constant->getZExtValue() == 15)
                    rewrittenSlot = store->getPointerOperand();
            }
        }

        report.expect(maskedSlot != nullptr, "IntRangeFacts: found the single-assignment slot");
        report.expect(rewrittenSlot != nullptr, "IntRangeFacts: found the rewritten slot");
        report.expect(!nswArithmetic.empty(), "IntRangeFacts: found wrap-flagged arithmetic");
        report.expect(!widenings.empty(), "IntRangeFacts: found an i32 to i64 widening");

        // publishSingleStoreSlots: the mask's bound must reach the slot and its loads.
        if (maskedSlot != nullptr)
        {
            const IntRange* slotRange = rangeOf(maskedSlot);
            report.expect(slotRange != nullptr,
                          "IntRangeFacts: single-assignment slot carries a range");
            if (slotRange != nullptr)
            {
                report.expect(slotRange->hasUpper && slotRange->upper == 255,
                              "IntRangeFacts: single-assignment slot is bounded above by 255");
                report.expect(slotRange->hasLower && slotRange->lower >= 0,
                              "IntRangeFacts: single-assignment slot is bounded below by 0");
            }

            unsigned boundedLoads = 0;
            for (const llvm::User* user : maskedSlot->users())
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                if (load == nullptr)
                    continue;
                const IntRange* loadRange = rangeOf(load);
                if (loadRange != nullptr && loadRange->hasUpper && loadRange->upper == 255)
                    ++boundedLoads;
            }
            report.expect(boundedLoads > 0,
                          "IntRangeFacts: loads of the single-assignment slot inherit the bound");
        }

        // The soundness guard: a slot written twice holds different values on different paths,
        // so neither it nor its loads may carry a range.
        if (rewrittenSlot != nullptr)
        {
            report.expect(rangeOf(rewrittenSlot) == nullptr,
                          "IntRangeFacts: rewritten slot carries no range");
            bool anyLoadBounded = false;
            for (const llvm::User* user : rewrittenSlot->users())
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                if (load != nullptr && rangeOf(load) != nullptr)
                    anyLoadBounded = true;
            }
            report.expect(!anyLoadBounded,
                          "IntRangeFacts: loads of the rewritten slot carry no range");
        }

        // restsOnWrapAssumption: no link of a wrap-flagged chain may be published, otherwise
        // the overflow check receives the assumption it exists to verify.
        bool anyWrapFlaggedBounded = false;
        for (const llvm::Instruction* instruction : nswArithmetic)
        {
            if (rangeOf(instruction) != nullptr)
                anyWrapFlaggedBounded = true;
        }
        report.expect(!anyWrapFlaggedBounded,
                      "IntRangeFacts: wrap-flagged arithmetic carries no range");

        // trivialRange: a bound that restates the source width bounds nothing.
        bool anyWideningBounded = false;
        for (const llvm::ZExtInst* zext : widenings)
        {
            const IntRange* range = rangeOf(zext);
            if (range != nullptr && range->hasUpper)
                anyWideningBounded = true;
        }
        report.expect(!anyWideningBounded,
                      "IntRangeFacts: an i32 to i64 widening carries no upper bound");

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
    /// Contract for library consumers (coretrace): the core returns a structured report and
    /// never writes it to stdout; rendering is a separate, pure step.
    bool testAnalysisReportContract(const std::filesystem::path& repoRoot, TestReport& report)
    {
        namespace app = ctrace::stack::app;
        namespace cli = ctrace::stack::cli;
        const std::filesystem::path source = repoRoot / "test/alloca/oversized-constant.c";

        cli::ParsedArguments args;
        args.inputFilenames = {source.string()};

        // Capture fd 1 around the run: any byte written there is a contract violation.
        std::fflush(stdout);
        llvm::outs().flush();
        char captureTemplate[] = "/tmp/ctrace-unit-stdout-XXXXXX";
        const int captureFd = mkstemp(captureTemplate);
        if (captureFd < 0)
        {
            report.expect(false, "AnalysisReport: unable to create stdout capture file");
            return false;
        }
        unlink(captureTemplate);
        const int savedStdout = dup(STDOUT_FILENO);
        dup2(captureFd, STDOUT_FILENO);

        app::ReportResult result = app::runAnalysis(std::move(args));

        std::fflush(stdout);
        llvm::outs().flush();
        dup2(savedStdout, STDOUT_FILENO);
        close(savedStdout);
        const off_t bytesOnStdout = lseek(captureFd, 0, SEEK_END);
        close(captureFd);

        report.expect(result.isOk(),
                      "AnalysisReport: runAnalysis succeeds on oversized-constant.c");
        report.expect(bytesOnStdout == 0, "AnalysisReport: runAnalysis writes nothing to stdout");
        if (!result.isOk())
        {
            std::cerr << "runAnalysis error: " << result.error << "\n";
            return false;
        }

        const app::AnalysisReport& analysis = *result.report;
        report.expect(analysis.contractVersion == 1, "AnalysisReport: contract version is 1");
        report.expect(analysis.files.size() == 1, "AnalysisReport: one file report per input");
        report.expect(analysis.inputFiles.size() == 1 &&
                          analysis.inputFiles.front() == source.string(),
                      "AnalysisReport: input file list is preserved");
        report.expect(analysis.summary.error == 1 && analysis.summary.warning == 0 &&
                          analysis.summary.info == 0,
                      "AnalysisReport: summary counts the single large-alloca error");
        report.expect(!analysis.files.empty() &&
                          analysis.files.front().summary.error == analysis.summary.error,
                      "AnalysisReport: per-file summary matches the total for a single input");
        report.expect(analysis.merged.diagnostics.size() == 1,
                      "AnalysisReport: merged result carries the diagnostic");
        if (!analysis.merged.diagnostics.empty())
        {
            // Read on the Diagnostic itself: consumers do not go through JSON or SARIF.
            const ctrace::stack::Diagnostic& diagnostic = analysis.merged.diagnostics.front();
            report.expect(diagnostic.ruleId == "AllocaTooLarge",
                          "AnalysisReport: the diagnostic names its rule");
            report.expect(diagnostic.cweId == "CWE-770",
                          "AnalysisReport: the diagnostic carries its CWE");
        }

        const std::string json = app::renderReport(analysis, cli::OutputFormat::Json);
        report.expect(json == ctrace::stack::toJson(analysis.merged, source.string()),
                      "renderReport(Json): equals toJson of the merged result");

        const std::string human = app::renderReport(analysis, cli::OutputFormat::Human);
        report.expect(human.rfind("Mode: IR\n", 0) == 0,
                      "renderReport(Human): starts with the mode line");
        report.expect(human.find("Function: big_alloca") != std::string::npos,
                      "renderReport(Human): lists the analyzed function");
        report.expect(human.find("Diagnostics summary: info=0, warning=0, error=1\n") !=
                          std::string::npos,
                      "renderReport(Human): contains the diagnostics summary line");

        const std::string sarif = app::renderReport(analysis, cli::OutputFormat::Sarif);
        report.expect(sarif == ctrace::stack::toSarif(analysis.merged, source.string(),
                                                      "coretrace-stack-analyzer", "0.1.0", ""),
                      "renderReport(Sarif): equals toSarif of the merged result");
        return true;
    }

    bool testUnresolvedCallsMarkStackUnknown(const std::filesystem::path& repoRoot,
                                             TestReport& report)
    {
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/unresolved_calls_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "UnresolvedCalls setup: failed to load module: " + loadError);
            return false;
        }

        ctrace::stack::analyzer::ModulePreparationService service;
        ctrace::stack::analyzer::PreparedModule prepared = service.prepare(*loaded.module, config);

        auto estimateOf = [&](const char* name) -> ctrace::stack::analysis::StackEstimate
        {
            const llvm::Function* fn = loaded.module->getFunction(name);
            if (!fn)
                return {};
            auto it = prepared.recursionState.TotalStack.find(fn);
            return it == prepared.recursionState.TotalStack.end()
                       ? ctrace::stack::analysis::StackEstimate{}
                       : it->second;
        };

        report.expect(estimateOf("calls_external").unknown,
                      "UnresolvedCalls: call to external declaration marks max stack unknown");
        report.expect(estimateOf("calls_indirect").unknown,
                      "UnresolvedCalls: indirect call marks max stack unknown");
        report.expect(!estimateOf("calls_defined").unknown,
                      "UnresolvedCalls: call to defined function keeps max stack known");
        report.expect(estimateOf("calls_defined").bytes > estimateOf("leaf").bytes,
                      "UnresolvedCalls: defined callee frame is still accumulated");
        report.expect(!estimateOf("calls_intrinsic_only").unknown,
                      "UnresolvedCalls: LLVM intrinsic call does not mark max stack unknown");
        report.expect(estimateOf("calls_caller_of_external").unknown,
                      "UnresolvedCalls: unknown propagates to callers");
        report.expect(estimateOf("calls_external").bytes > 0,
                      "UnresolvedCalls: lower bound keeps the known local frame");

        return report.failures == 0;
    }

    bool testAssumeExternalFrameReplacesUnknown(const std::filesystem::path& repoRoot,
                                                TestReport& report)
    {
        ctrace::stack::AnalysisConfig config;
        config.assumeExternalFrame = 1;
        config.assumeExternalFrameBytes = 512;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/unresolved_calls_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "AssumeExternalFrame setup: failed to load module: " + loadError);
            return false;
        }

        ctrace::stack::analyzer::ModulePreparationService service;
        ctrace::stack::analyzer::PreparedModule prepared = service.prepare(*loaded.module, config);

        auto totalOf = [&](const char* name) -> ctrace::stack::analysis::StackEstimate
        {
            const llvm::Function* fn = loaded.module->getFunction(name);
            auto it = fn ? prepared.recursionState.TotalStack.find(fn)
                         : prepared.recursionState.TotalStack.end();
            return it == prepared.recursionState.TotalStack.end()
                       ? ctrace::stack::analysis::StackEstimate{}
                       : it->second;
        };
        auto localOf = [&](const char* name) -> ctrace::stack::StackSize
        {
            const llvm::Function* fn = loaded.module->getFunction(name);
            auto it = fn ? prepared.localStack.find(fn) : prepared.localStack.end();
            return it == prepared.localStack.end() ? 0 : it->second.bytes;
        };

        report.expect(!totalOf("calls_external").unknown,
                      "AssumeExternalFrame: external call no longer marks max stack unknown");
        report.expect(totalOf("calls_external").bytes == localOf("calls_external") + 512,
                      "AssumeExternalFrame: external call costs the assumed frame");
        report.expect(!totalOf("calls_indirect").unknown,
                      "AssumeExternalFrame: indirect call no longer marks max stack unknown");
        report.expect(totalOf("calls_indirect").bytes == localOf("calls_indirect") + 512,
                      "AssumeExternalFrame: indirect call costs the assumed frame");
        report.expect(totalOf("calls_caller_of_external").bytes ==
                          localOf("calls_caller_of_external") + localOf("calls_external") + 512,
                      "AssumeExternalFrame: assumed frame propagates through callers");
        report.expect(totalOf("calls_intrinsic_only").bytes == localOf("calls_intrinsic_only"),
                      "AssumeExternalFrame: intrinsics are not charged");
        report.expect(totalOf("calls_defined").bytes == localOf("calls_defined") + localOf("leaf"),
                      "AssumeExternalFrame: defined callees keep their real frame");

        return report.failures == 0;
    }
    bool testUninitializedFixpointBudgetIsExplicit(const std::filesystem::path& repoRoot,
                                                   TestReport& report)
    {
        using namespace ctrace::stack::analysis;

        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/uninit_fixpoint_budget_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "UninitFixpointBudget setup: failed to load module: " + loadError);
            return false;
        }
        auto analyzeAll = [](const llvm::Function&) { return true; };

        const auto countKind = [](const std::vector<UninitializedLocalReadIssue>& issues,
                                  UninitializedLocalIssueKind kind, const char* func)
        {
            std::size_t count = 0;
            for (const UninitializedLocalReadIssue& issue : issues)
                if (issue.kind == kind && issue.funcName == func)
                    ++count;
            return count;
        };

        // Automatic budget: converges, no incompleteness reported.
        {
            const std::vector<UninitializedLocalReadIssue> issues =
                analyzeUninitializedLocalReads(*loaded.module, analyzeAll, nullptr);
            report.expect(
                countKind(issues, UninitializedLocalIssueKind::AnalysisIncomplete,
                          "reads_uninit") == 0 &&
                    countKind(issues, UninitializedLocalIssueKind::AnalysisIncomplete, "fill") == 0,
                "UninitFixpointBudget: converged analysis reports no incompleteness");
            report.expect(countKind(issues, UninitializedLocalIssueKind::ReadBeforeDefiniteInit,
                                    "reads_uninit") == 1,
                          "UninitFixpointBudget: converged analysis finds the read of x");

            const UninitializedSummaryIndex summaries = buildUninitializedSummaryIndex(
                *loaded.module, analyzeAll, static_cast<const UninitializedSummaryIndex*>(nullptr));
            const auto it =
                summaries.functions.find(linkerSymbolName(*loaded.module->getFunction("fill")));
            const bool claimsWrite = it != summaries.functions.end() &&
                                     !it->second.paramEffects.empty() &&
                                     !it->second.paramEffects[0].writeRanges.empty() &&
                                     !it->second.paramEffects[0].hasUnknownWrite;
            report.expect(
                claimsWrite,
                "UninitFixpointBudget: converged summary of fill claims a definite write");
        }

        // Budget of one iteration: loops cannot converge, and it must be said.
        {
            const std::vector<UninitializedLocalReadIssue> issues =
                analyzeUninitializedLocalReads(*loaded.module, analyzeAll, nullptr,
                                               /*fixpointIterationLimit=*/1);
            report.expect(countKind(issues, UninitializedLocalIssueKind::AnalysisIncomplete,
                                    "reads_uninit") == 1,
                          "UninitFixpointBudget: exhausted budget reports AnalysisIncomplete");
            report.expect(
                countKind(issues, UninitializedLocalIssueKind::AnalysisIncomplete, "fill") == 1,
                "UninitFixpointBudget: every non-converged function is reported once");
            report.expect(countKind(issues, UninitializedLocalIssueKind::ReadBeforeDefiniteInit,
                                    "reads_uninit") == 1,
                          "UninitFixpointBudget: issues found before exhaustion are kept");

            const UninitializedSummaryIndex summaries = buildUninitializedSummaryIndex(
                *loaded.module, analyzeAll, static_cast<const UninitializedSummaryIndex*>(nullptr),
                /*fixpointIterationLimit=*/1);
            const auto it =
                summaries.functions.find(linkerSymbolName(*loaded.module->getFunction("fill")));
            const bool downgraded = it != summaries.functions.end() &&
                                    !it->second.paramEffects.empty() &&
                                    it->second.paramEffects[0].writeRanges.empty() &&
                                    it->second.paramEffects[0].pointerSlotWrites.empty() &&
                                    it->second.paramEffects[0].hasUnknownWrite;
            report.expect(
                downgraded,
                "UninitFixpointBudget: non-converged summary downgrades writes to unknown");

            // The emitter renders it as an Info diagnostic under a rule of its own (an analysis
            // limit, not a finding), so it is visible in JSON/SARIF but hidden by --warnings-only.
            ctrace::stack::AnalysisResult rendered;
            ctrace::stack::analyzer::appendUninitializedLocalReadDiagnostics(rendered, issues);
            std::size_t infoCount = 0;
            bool infoMentionsBudget = false;
            for (const ctrace::stack::Diagnostic& diag : rendered.diagnostics)
            {
                if (diag.severity != ctrace::stack::DiagnosticSeverity::Info)
                    continue;
                ++infoCount;
                infoMentionsBudget = infoMentionsBudget ||
                                     (diag.message.find("did not converge") != std::string::npos &&
                                      diag.ruleId == "UninitializedLocalRead.AnalysisIncomplete");
            }
            report.expect(infoCount == 2 && infoMentionsBudget,
                          "UninitFixpointBudget: AnalysisIncomplete renders as an Info diagnostic");
        }

        return report.failures == 0;
    }
    /// Program-point ranges: a branch constraint must hold only where its edge dominates.
    bool testLinkerSymbolName(TestReport& report)
    {
        using ctrace::stack::analysis::linkerSymbolName;
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        const auto parse = [&](const std::string& dataLayout)
        {
            return llvm::parseAssemblyString(
                "target datalayout = \"" + dataLayout +
                    "\"\n"
                    "declare void @plain()\n"
                    "declare void @\"\\01verbatim\"()\n"
                    "declare void @_ZN5probe4peekEPNSt3__13tagE()\n"
                    "declare void @_ZN5probe4peekEPNSt7__cxx113tagE()\n",
                error, context);
        };
        const std::unique_ptr<llvm::Module> elf = parse("e-m:e-i64:64-n32:64-S128");
        const std::unique_ptr<llvm::Module> macho = parse("e-m:o-i64:64-n32:64-S128");
        if (!elf || !macho)
        {
            report.expect(false, "LinkerSymbolName setup: the IR does not parse");
            return false;
        }
        report.expect(linkerSymbolName(*elf->getFunction("plain")) == "plain",
                      "LinkerSymbolName: ELF adds no prefix");
        report.expect(linkerSymbolName(*macho->getFunction("plain")) == "_plain",
                      "LinkerSymbolName: Mach-O adds its underscore");
        report.expect(linkerSymbolName(*macho->getFunction("\1verbatim")) == "verbatim",
                      "LinkerSymbolName: an assembler name is taken as written");
        report.expect(linkerSymbolName(*elf->getFunction("_ZN5probe4peekEPNSt3__13tagE")) !=
                          linkerSymbolName(*elf->getFunction("_ZN5probe4peekEPNSt7__cxx113tagE")),
                      "LinkerSymbolName: std::__1 and std::__cxx11 symbols stay distinct");
        return report.failures == 0;
    }

    /// #217: the code generated before and after a const rewrite is compared once the symbols
    /// the rewrite renames are paired, by the pairs its correspondences established and by them
    /// only: a symbol renamed without a pair, even one whose name differs only by const, is a
    /// difference.
    bool testConstSuggestionCodeComparison(TestReport& report)
    {
        using ctrace::stack::analysis::rewrittenCodeDifference;
        using ctrace::stack::analysis::SymbolPair;
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        const auto parse =
            [&](std::string text, const std::string& function, const std::string& typeInfo)
        {
            for (const auto& [from, to] : {std::pair{std::string("@F"), "@" + function},
                                           std::pair{std::string("@TI"), "@" + typeInfo}})
            {
                for (std::size_t at = text.find(from); at != std::string::npos;
                     at = text.find(from, at + to.size()))
                {
                    text.replace(at, from.size(), to);
                }
            }
            return llvm::parseAssemblyString(text, error, context);
        };
        // The rewritten function and its caller.
        const std::string called = "define i32 @F(ptr %p) {\n"
                                   "  %v = load i32, ptr %p\n"
                                   "  ret i32 %v\n"
                                   "}\n"
                                   "define i32 @_Z3usePi(ptr %p) {\n"
                                   "  %r = call i32 @F(ptr %p)\n"
                                   "  ret i32 %r\n"
                                   "}\n";
        // The rewritten function throws its parameter: the thrown type changes with it, and a
        // handler `catch (int*)` elsewhere no longer catches it.
        const std::string thrown = "@TI = external constant ptr\n"
                                   "declare ptr @__cxa_allocate_exception(i64)\n"
                                   "declare void @__cxa_throw(ptr, ptr, ptr)\n"
                                   "define void @F(ptr %p) {\n"
                                   "  %e = call ptr @__cxa_allocate_exception(i64 8)\n"
                                   "  store ptr %p, ptr %e\n"
                                   "  call void @__cxa_throw(ptr %e, ptr @TI, ptr null)\n"
                                   "  unreachable\n"
                                   "}\n";
        const std::unique_ptr<llvm::Module> calledBefore = parse(called, "_Z1fPi", "");
        const std::unique_ptr<llvm::Module> calledAfter = parse(called, "_Z1fPKi", "");
        const std::unique_ptr<llvm::Module> thrownBefore = parse(thrown, "_Z1fPi", "_ZTIPi");
        const std::unique_ptr<llvm::Module> thrownAfter = parse(thrown, "_Z1fPKi", "_ZTIPKi");
        if (!calledBefore || !calledAfter || !thrownBefore || !thrownAfter)
        {
            report.expect(false, "ConstSuggestionCode setup: the IR does not parse");
            return false;
        }
        const std::vector<SymbolPair> function = {{"_Z1fPi", "_Z1fPKi"}};
        report.expect(!rewrittenCodeDifference(*calledBefore, *calledAfter, function),
                      "ConstSuggestionCode: the rewritten function and its caller are renamed "
                      "together");
        report.expect(rewrittenCodeDifference(*calledBefore, *calledAfter, {}).has_value(),
                      "ConstSuggestionCode: a function renamed without an established pair is a "
                      "difference");
        report.expect(rewrittenCodeDifference(*thrownBefore, *thrownAfter, function).has_value(),
                      "ConstSuggestionCode: typeinfo for int* and for int const*, without a pair, "
                      "are not paired");
        return report.failures == 0;
    }

    /// #217: the code comparison sets aside what carries no meaning for an execution: debug
    /// information, diagnostic metadata, the descriptive names of alias scopes, the numbers of
    /// metadata and the comments of the printed IR. Every other metadata is compared by contents.
    bool testConstSuggestionNormalizations(TestReport& report)
    {
        using ctrace::stack::analysis::rewrittenCodeDifference;
        llvm::LLVMContext context;
        llvm::SMDiagnostic error;
        const auto differ = [&](const std::string& before, const std::string& after)
        {
            const std::unique_ptr<llvm::Module> was =
                llvm::parseAssemblyString(before, error, context);
            const std::unique_ptr<llvm::Module> now =
                llvm::parseAssemblyString(after, error, context);
            if (!was || !now)
                return std::optional<bool>();
            return std::optional<bool>(rewrittenCodeDifference(*was, *now, {}).has_value());
        };
        const auto replaced = [](std::string text, const std::string& from, const std::string& to)
        {
            text.replace(text.find(from), from.size(), to);
            return text;
        };
        const std::string range = "define i32 @f(ptr %p) {\n"
                                  "  %v = load i32, ptr %p, !range !0\n"
                                  "  ret i32 %v\n"
                                  "}\n"
                                  "!0 = !{i32 0, i32 10}\n";
        const std::string debug =
            "define i32 @f(ptr %p) !dbg !3 {\n"
            "  %v = load i32, ptr %p, !dbg !6\n"
            "  ret i32 %v, !dbg !6\n"
            "}\n"
            "!llvm.dbg.cu = !{!0}\n"
            "!llvm.module.flags = !{!2}\n"
            "!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, emissionKind: "
            "FullDebug)\n"
            "!1 = !DIFile(filename: \"a.c\", directory: \"/\")\n"
            "!2 = !{i32 2, !\"Debug Info Version\", i32 3}\n"
            "!3 = distinct !DISubprogram(name: \"f\", scope: !1, file: !1, line: 1, type: !4, "
            "spFlags: DISPFlagDefinition, unit: !0)\n"
            "!4 = !DISubroutineType(types: !5)\n"
            "!5 = !{}\n"
            "!6 = !DILocation(line: 2, column: 3, scope: !3)\n";
        const std::string srcloc = "define void @f() {\n"
                                   "  call void asm sideeffect \"nop\", \"\"(), !srcloc !0\n"
                                   "  ret void\n"
                                   "}\n"
                                   "!0 = !{i64 100}\n";
        const std::string annotation = "define void @f() {\n"
                                       "  %a = alloca i32, align 4, !annotation !0\n"
                                       "  ret void\n"
                                       "}\n"
                                       "!0 = !{!\"auto-init\"}\n";
        const std::string scopes =
            "declare void @llvm.experimental.noalias.scope.decl(metadata)\n"
            "define void @f(ptr %p, ptr %q) {\n"
            "  call void @llvm.experimental.noalias.scope.decl(metadata !2)\n"
            "  %v = load i32, ptr %p, align 4, !alias.scope !2, !noalias !3\n"
            "  store i32 %v, ptr %q, align 4, !alias.scope !3, !noalias !2\n"
            "  ret void\n"
            "}\n"
            "!0 = distinct !{!0, !\"domain of f(int*)\"}\n"
            "!1 = distinct !{!1, !0, !\"f(int*): %p\"}\n"
            "!4 = distinct !{!4, !0, !\"f(int*): %q\"}\n"
            "!2 = !{!1}\n"
            "!3 = !{!4}\n";
        const std::string flags = "define void @f() {\n"
                                  "  ret void\n"
                                  "}\n"
                                  "!llvm.module.flags = !{!0}\n"
                                  "!0 = !{i32 1, !\"wchar_size\", i32 4}\n";
        // The same two functions, defined in the other order: the printer numbers their loop
        // metadata in the other order.
        const std::string loopF = "define void @f() {\n"
                                  "entry:\n"
                                  "  br label %a\n"
                                  "a:\n"
                                  "  br label %a, !llvm.loop !0\n"
                                  "}\n";
        const std::string loopG = "define void @g() {\n"
                                  "entry:\n"
                                  "  br label %b\n"
                                  "b:\n"
                                  "  br label %b, !llvm.loop !1\n"
                                  "}\n";
        const std::string loops = "!0 = distinct !{!0, !2}\n"
                                  "!1 = distinct !{!1, !3}\n"
                                  "!2 = !{!\"llvm.loop.mustprogress\"}\n"
                                  "!3 = !{!\"llvm.loop.unroll.disable\"}\n";
        const std::optional<bool> orderChanged =
            differ(loopF + loopG + loops, loopG + loopF + loops);
        // The same code, its join block's predecessors listed in the other order: only the
        // "; preds" comment of the printed IR changes.
        const std::string join = "define i32 @f(i1 %c) {\n"
                                 "entry:\n"
                                 "  br i1 %c, label %a, label %b\n"
                                 "a:\n"
                                 "  br label %join\n"
                                 "b:\n"
                                 "  br label %join\n"
                                 "join:\n"
                                 "  %v = phi i32 [ 1, %a ], [ 2, %b ]\n"
                                 "  ret i32 %v\n"
                                 "}\n";
        const std::optional<bool> predecessorsChanged =
            differ(join, replaced(join, "  ret i32 %v\n",
                                  "  ret i32 %v\n  uselistorder label %join, { 1, 0 }\n"));
        const std::optional<bool> rangeChanged =
            differ(range, replaced(range, "i32 0, i32 10", "i32 0, i32 5"));
        const std::optional<bool> debugChanged =
            differ(debug, replaced(debug, "column: 3", "column: 9"));
        const std::optional<bool> srclocChanged =
            differ(srcloc, replaced(srcloc, "i64 100", "i64 200"));
        const std::optional<bool> annotationChanged =
            differ(annotation, replaced(annotation, "auto-init", "other"));
        const std::optional<bool> namesChanged = differ(
            scopes,
            replaced(replaced(replaced(scopes, "domain of f(int*)", "domain of f(const int*)"),
                              "f(int*): %p", "f(const int*): %p"),
                     "f(int*): %q", "f(const int*): %q"));
        const std::optional<bool> scopesChanged =
            differ(scopes, replaced(scopes, "!alias.scope !3, !noalias !2",
                                    "!alias.scope !2, !noalias !2"));
        const std::optional<bool> flagsChanged = differ(flags, replaced(flags, "i32 4}", "i32 2}"));
        if (!orderChanged || !predecessorsChanged || !rangeChanged || !debugChanged ||
            !srclocChanged || !annotationChanged || !namesChanged || !scopesChanged ||
            !flagsChanged)
        {
            report.expect(false, "ConstSuggestionMetadata setup: the IR does not parse: " +
                                     error.getMessage().str());
            return false;
        }
        report.expect(!*orderChanged,
                      "ConstSuggestionMetadata: metadata numbered in another order are the same");
        report.expect(!*predecessorsChanged,
                      "ConstSuggestionCode: predecessors listed in another order are the same");
        report.expect(*rangeChanged, "ConstSuggestionMetadata: another !range is a difference");
        report.expect(!*debugChanged, "ConstSuggestionMetadata: debug information is not compared");
        report.expect(!*srclocChanged,
                      "ConstSuggestionMetadata: the source position of inline assembly is not "
                      "compared");
        report.expect(!*annotationChanged, "ConstSuggestionMetadata: annotations are not compared");
        report.expect(!*namesChanged,
                      "ConstSuggestionMetadata: the descriptive names of alias scopes are not "
                      "compared");
        report.expect(*scopesChanged,
                      "ConstSuggestionMetadata: another alias scope on an access is a difference");
        report.expect(*flagsChanged,
                      "ConstSuggestionMetadata: another module flag is a difference");
        return report.failures == 0;
    }

    /// #217 (D10, D18): step 3 detects a rewrite that loses the parameter's own top-level const.
    /// The plan of read_only(int *const p) is executed with an edit that writes `const int *`
    /// instead of `const int *const`: the declaration does not get the candidate of its type,
    /// whatever the planner's later edits are.
    bool testConstSuggestionTopLevelQualifierLost(TestReport& report)
    {
        namespace cs = ctrace::stack::analysis::const_suggestion;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "ct_const_suggestion_top_level_lost";
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "k.c";
        const std::string text = "int read_only(int *const p) { return *p; }\n";
        std::ofstream(file) << text;
        const ctrace::stack::analysis::UnitCommand command{{"clang", "-c", file.string()},
                                                           dir.string()};
        std::vector<cs::Target> targets(1);
        targets[0].symbol = "read_only";
        targets[0].sourceIndex = 0;
        const cs::UnitFacts before = cs::inspect(command, nullptr, {}, targets, "test", false);
        if (before.entities.size() != 1 || before.entities[0].plan.edits.size() != 1)
        {
            report.expect(false, "ConstSuggestionTopLevel setup: read_only is not planned");
            return false;
        }
        cs::SuggestionPlan lost = before.entities[0].plan;
        const std::string written = "int *const";
        const std::size_t at = text.find(written);
        lost.edits.begin()->second = {{at, written.size(), "const int *", 1}};
        std::string rewritten = text;
        rewritten.replace(at, written.size(), "const int *");
        cs::Target target = targets[0];
        target.plan = &lost;
        target.expected = before.entities[0].expected;
        const cs::UnitFacts after = cs::inspect(
            command, &lost, {{lost.edits.begin()->first, rewritten}}, {target}, "test", false);
        report.expect(after.entities.size() == 1 && after.entities[0].withoutCandidate,
                      "ConstSuggestionTopLevel: a rewrite that loses the pointer's own const does "
                      "not get the candidate");
        return report.failures == 0;
    }

    /// A compile command for a C++ source, as the Clang check builds one: compilerlib's driver
    /// settings (Clang's resource directory, the SDK on macOS) before the arguments, so that the
    /// standard library's headers are found on every platform.
    ctrace::stack::analysis::UnitCommand cxxCommand(const std::filesystem::path& file)
    {
        const std::vector<std::string> args{"-std=c++20", "-c", file.string()};
        ctrace::stack::analysis::UnitCommand out{{"clang++"}, file.parent_path().string()};
        compilerlib::DriverConfig driver;
        std::string error;
        if (compilerlib::resolveDriverConfig(args, driver, error))
        {
            out.args = {driver.clang_path};
            if (driver.force_cxx_driver)
                out.args.push_back("--driver-mode=g++");
            if (driver.add_resource_dir)
            {
                out.args.push_back("-resource-dir");
                out.args.push_back(driver.resource_dir);
            }
            if (driver.add_sysroot)
            {
                out.args.push_back("-isysroot");
                out.args.push_back(driver.sysroot);
            }
        }
        out.args.insert(out.args.end(), args.begin(), args.end());
        return out;
    }

    // run_test.py's P2_KEYS_SOURCE with a polymorphic local class, whose destructor has a deleting
    // and a base variant.
    constexpr const char* kNestedKeysSourceWithPoly = R"(#include <vector>

template <typename T> struct Box
{
    T* value;
    friend bool operator==(const Box& a, const Box& b) { return a.value == b.value; }
    struct Inner
    {
        int n = 0;
    };
};

template <typename T> int probe(T) { return 1; }

template <typename L> int apply_both(L call)
{
    int (*pointer)(int) = call;
    return pointer(1) + call(1);
}

template <typename Each> int pass(int n, Each&& each)
{
    int total = 0;
    each(n, [&](int i) { total += i; });
    return total;
}

int drive(int n)
{
    return pass(n, [&](int count, auto&& body) {
        struct State
        {
            std::vector<int> v;
        };
        State a{{count}};
        State b = a;
        State c = static_cast<State&&>(a);
        b = c;
        c = static_cast<State&&>(b);
        State d;
        struct Poly
        {
            virtual ~Poly() {}
        };
        delete new Poly;
        Box<State> x{nullptr}, y{nullptr};
        const int found = (x == y) + probe(typename Box<State>::Inner{}) +
                          probe(typename Box<decltype([](int v) { return v; })>::Inner{}) +
                          probe(typename Box<decltype([](int v) { return -v; })>::Inner{}) +
                          apply_both([](int v) { return 2 * v; });
        body(found + static_cast<int>(b.v.size() + c.v.size() + d.v.size()));
    });
}
)";

    /// #217 (§5.7, step 5): the correspondences of the types nested in established pairs and of the
    /// specializations whose arguments hold them (§5.7): only the symbols that the rewrite renames,
    /// one to one, every other part of a key kept. Synthetic facts: what Clang writes into a key is
    /// pinned by testConstSuggestionNestedIdentities.
    bool testConstSuggestionNestedCorrespondences(TestReport& report)
    {
        namespace cs = ctrace::stack::analysis::const_suggestion;
        // The entity before and after the rewrite, paired by its variant (step 1).
        const std::string was = "_Z1fIiEviOT_";
        const std::string now = "_Z1fIiEviRKT_";
        // A type nested in a function: its record's site and the symbol of the function around it.
        const auto nested = [](const std::string& site, const std::string& around)
        { return "\x01" + site + "\x02" + around + "\x03"; };
        // A specialization's key: variant, pattern site, its arguments, its function type.
        const auto spec =
            [&](const std::string& variant, const std::string& argument, const std::string& other)
        {
            return variant + "|use.cpp:3|(" + argument + ", " + other + ")[void (" + argument +
                   " &&)]";
        };
        const auto pairsOf = [&](std::vector<cs::UnitFacts::Identity> before,
                                 std::vector<cs::UnitFacts::Identity> after, std::string& missing)
        {
            cs::UnitFacts wasUnit;
            wasUnit.entities.resize(1);
            wasUnit.entities[0].variants = {{was, "", ""}};
            wasUnit.identities = std::move(before);
            cs::UnitFacts nowUnit;
            nowUnit.entities.resize(1);
            nowUnit.entities[0].variants = {{now, "", ""}};
            nowUnit.identities = std::move(after);
            std::vector<std::string> out;
            for (const ctrace::stack::analysis::SymbolPair& pair :
                 cs::correspond(wasUnit, 0, nowUnit, missing))
            {
                if (pair.before != was)
                    out.push_back(pair.before + "->" + pair.after);
            }
            return out;
        };
        using Pairs = std::vector<std::string>;
        const std::string inWas = nested("use.cpp:40", was);
        const std::string inNow = nested("use.cpp:40", now);
        std::string missing;

        // A specialization whose argument is a type nested in the entity pairs through the
        // entity's pair.
        report.expect(pairsOf({{"_Z1aOld", "", "", spec("", inWas, "int")}},
                              {{"_Z1aNew", "", "", spec("", inNow, "int")}},
                              missing) == Pairs{"_Z1aOld->_Z1aNew"} &&
                          missing.empty(),
                      "ConstSuggestionNested: a specialization pairs through its nested argument");

        // Every other argument is kept: int against long, no pair (mutant: a key without the
        // arguments that hold no nested type).
        missing.clear();
        report.expect(pairsOf({{"_Z1aOld", "", "", spec("", inWas, "int")}},
                              {{"_Z1aNew", "", "", spec("", inNow, "long")}}, missing)
                          .empty(),
                      "ConstSuggestionNested: another argument differs, no pair");

        // Refinement 1, only renamed symbols: a symbol present on both sides at the same key
        // stays out, and the renamed one pairs (without that filter: two candidates on each
        // side, no pair, a reason). The nested type is written in a template, without an
        // enclosing symbol, so its key does not change with the rewrite.
        const std::string inTemplate = nested("use.cpp:50", "");
        missing.clear();
        report.expect(pairsOf({{"_Z1bOld", "", "", spec("", inTemplate, "int")},
                               {"_Z1bSame", "", "", spec("", inTemplate, "int")}},
                              {{"_Z1bNew", "", "", spec("", inTemplate, "int")},
                               {"_Z1bSame", "", "", spec("", inTemplate, "int")}},
                              missing) == Pairs{"_Z1bOld->_Z1bNew"} &&
                          missing.empty(),
                      "ConstSuggestionNested: a symbol the rewrite does not rename takes no part");

        // One to one: two before and two after at one key, no pair and a reason, not the
        // first candidates paired.
        missing.clear();
        report.expect(pairsOf({{"_Z1cOne", "", "", spec("", inWas, "int")},
                               {"_Z1cTwo", "", "", spec("", inWas, "int")}},
                              {{"_Z1cThree", "", "", spec("", inNow, "int")},
                               {"_Z1cFour", "", "", spec("", inNow, "int")}},
                              missing)
                              .empty() &&
                          missing.find("has no one-to-one counterpart") != std::string::npos,
                      "ConstSuggestionNested: two candidates on each side, no pair, a reason");

        // One before, two after: no pair, a reason.
        missing.clear();
        report.expect(pairsOf({{"_Z1dOne", "", "", spec("", inWas, "int")}},
                              {{"_Z1dTwo", "", "", spec("", inNow, "int")},
                               {"_Z1dThree", "", "", spec("", inNow, "int")}},
                              missing)
                              .empty() &&
                          missing.find("has no one-to-one counterpart") != std::string::npos,
                      "ConstSuggestionNested: one candidate becomes two, no pair, a reason");

        // The ABI variant is part of the key: C1 before, C2 after, no pair.
        missing.clear();
        report.expect(pairsOf({{"_Z1eOld", "", "", spec("C1", inWas, "int")}},
                              {{"_Z1eNew", "", "", spec("C2", inNow, "int")}}, missing)
                          .empty(),
                      "ConstSuggestionNested: two variants never pair");

        // A member of a nested type pairs once the function around it is paired; around a
        // function without a pair, it does not.
        const std::string member = "lambda |use.cpp:40|use.cpp:40|operator() const|int (int)";
        missing.clear();
        report.expect(pairsOf({{"_ZOldL", member, was, ""}}, {{"_ZNewL", member, now, ""}},
                              missing) == Pairs{"_ZOldL->_ZNewL"},
                      "ConstSuggestionNested: a lambda's member pairs through its function");
        missing.clear();
        report.expect(
            pairsOf({{"_ZOldL", member, "_Z1hv", ""}}, {{"_ZNewL", member, now, ""}}, missing)
                .empty(),
            "ConstSuggestionNested: around an unpaired function, no pair");
        return report.failures == 0;
    }

    /// #217 (§5.7, step 5): what Clang writes into the keys of the nested entities, on one compilation of
    /// real code. Every expected member and specialization has an identity, and two symbols
    /// never share a key: a local class's implicit members, a lambda's call operator and static
    /// invoker, a virtual destructor's variants, a class template's hidden friend, and member
    /// classes of two specializations that differ only by a closure type.
    bool testConstSuggestionNestedIdentities(TestReport& report)
    {
        namespace cs = ctrace::stack::analysis::const_suggestion;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "ct_const_suggestion_nested_identities";
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "keys.cpp";
        // checks.py's P2_KEYS_SOURCE, with a polymorphic local class for the destructor's
        // variants (its vtable is a global that P2 does not pair, so the end-to-end check
        // leaves it out).
        std::ofstream(file) << kNestedKeysSourceWithPoly;
        const ctrace::stack::analysis::UnitCommand command = cxxCommand(file);
        std::vector<cs::Target> targets(1);
        targets[0].symbol = "_Z4passIZ5driveiE3$_0EiiOT_";
        targets[0].sourceIndex = 0;
        const cs::UnitFacts unit = cs::inspect(command, nullptr, {}, targets, "test", false);
        // How many identities a mangled or demangled part selects, by kind of key.
        const auto count = [&](const std::string& part, bool specialization)
        {
            return std::count_if(unit.identities.begin(), unit.identities.end(),
                                 [&](const cs::UnitFacts::Identity& identity)
                                 {
                                     const std::string& key =
                                         specialization ? identity.specialization : identity.nested;
                                     return !key.empty() &&
                                            (identity.symbol.find(part) != std::string::npos ||
                                             llvm::demangle(identity.symbol).find(part) !=
                                                 std::string::npos);
                                 });
        };
        // Presence: each refinement's candidates are identified: the
        // default, copy and move constructors (State d, b, c), the two assignments.
        report.expect(count("::State::State(", false) >= 3 &&
                          count("::State::operator=(", false) == 2,
                      "ConstSuggestionNestedKeys: the local class's implicit members are there");
        report.expect(count("__invoke", false) == 1, "ConstSuggestionNestedKeys: the static "
                                                     "invoker is there");
        report.expect(count("4PolyD0Ev", false) == 1 && count("4PolyD2Ev", false) == 1,
                      "ConstSuggestionNestedKeys: the destructor's deleting and base variants "
                      "are there");
        report.expect(count("operator==", true) == 1,
                      "ConstSuggestionNestedKeys: the hidden friend is identified (by its "
                      "function type)");
        report.expect(count("::Inner>(", true) == 3,
                      "ConstSuggestionNestedKeys: probe of the three member classes is identified "
                      "(through their specialization, which holds the nested type)");
        // Distinctness: no two symbols share a key (without the member's type, the implicit
        // constructors and assignments merge; without the name, the static flag and the
        // qualifiers together, which the test protects collectively, not each one, operator() and
        // __invoke merge; without the variant, D0 and D2 merge; a member
        // class written without its specialization's arguments: probe's two closures merge).
        std::map<std::string, std::string> owner;
        bool apart = true;
        for (const cs::UnitFacts::Identity& identity : unit.identities)
        {
            for (const std::string& key :
                 {identity.nested.empty() ? "" : "n" + identity.nested + '\0' + identity.enclosing,
                  identity.specialization.empty() ? "" : "s" + identity.specialization})
            {
                if (key.empty())
                    continue;
                const auto [it, added] = owner.try_emplace(key, identity.symbol);
                apart = apart && (added || it->second == identity.symbol);
            }
        }
        report.expect(apart, "ConstSuggestionNestedKeys: two symbols never share a key");
        return report.failures == 0;
    }

    // The implicit members of classes whose arguments hold a type nested in the generic lambda's
    // call operator (P2, implicit-member extension): Box<State>'s six, Derived<State>'s virtual
    // destructor (D0 and D2), Holder<State, int>'s and Holder<State, long>'s destructors; and a
    // class whose arguments hold no nested type, Plain<int>, whose members must not be identified.
    constexpr const char* kImplicitMembersSource = R"(#include <vector>

struct Base
{
    virtual ~Base() {}
};

template <typename T> struct Box
{
    T value;
};

template <typename T> struct Derived : Base
{
    T value;
};

template <typename T, typename U> struct Holder
{
    T value;
    U other;
};

template <typename T> struct Plain
{
    std::vector<T> items;
};

template <typename Each> int pass(int n, Each&& each)
{
    int total = 0;
    each(n, [&](int i) { total += i; });
    return total;
}

int drive(int n)
{
    return pass(n, [&](int count, auto&& body) {
        struct State
        {
            std::vector<int> v;
        };
        Box<State> a{{{count}}};
        Box<State> b = a;
        Box<State> c = static_cast<Box<State>&&>(a);
        b = c;
        c = static_cast<Box<State>&&>(b);
        Box<State> d;
        delete new Derived<State>;
        Holder<State, int> h{};
        Holder<State, long> k{};
        Plain<int> plain;
        body(static_cast<int>(b.value.v.size() + c.value.v.size() + d.value.v.size() +
                              plain.items.size()) +
             h.other + static_cast<int>(k.other));
    });
}
)";

    /// #217 (§5.7, step 5): an implicit member of a class whose key holds a nested type is
    /// identified by that class, its kind and name, its variant and its whole type. Every expected
    /// one is there, a class without a nested type gets none, and no two symbols share a key: the
    /// copy and move constructors (the type), D0 and D2 (the variant), Holder<State, int> and
    /// Holder<State, long> (the class's other arguments). Limit: the kind and the name are not
    /// shown to be needed. On every real case found, the variant (C against D) or the type already
    /// separates two implicit members, so a key without them passes these tests too; they are kept,
    /// and protected only together with the variant and the type.
    bool testConstSuggestionImplicitMemberIdentities(TestReport& report)
    {
        namespace cs = ctrace::stack::analysis::const_suggestion;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path() / "ct_const_suggestion_implicit_members";
        std::filesystem::create_directories(dir);
        const std::filesystem::path file = dir / "implicit.cpp";
        std::ofstream(file) << kImplicitMembersSource;
        const ctrace::stack::analysis::UnitCommand command = cxxCommand(file);
        std::vector<cs::Target> targets(1);
        targets[0].symbol = "_Z4passIZ5driveiE3$_0EiiOT_";
        targets[0].sourceIndex = 0;
        const cs::UnitFacts unit = cs::inspect(command, nullptr, {}, targets, "test", false);
        // The implicit-member keys whose symbol holds a mangled or demangled part.
        const auto count = [&](const std::string& part)
        {
            return std::count_if(unit.identities.begin(), unit.identities.end(),
                                 [&](const cs::UnitFacts::Identity& identity)
                                 {
                                     return identity.specialization.rfind("implicit ", 0) == 0 &&
                                            (identity.symbol.find(part) != std::string::npos ||
                                             llvm::demangle(identity.symbol).find(part) !=
                                                 std::string::npos);
                                 });
        };
        report.expect(count("Box<") >= 6,
                      "ConstSuggestionImplicitMembers: Box<State>'s default, copy and move "
                      "constructors, assignments and destructor are identified");
        report.expect(count("DerivedI") >= 2 && count("D0Ev") >= 1 && count("D2Ev") >= 1,
                      "ConstSuggestionImplicitMembers: Derived<State>'s deleting and base "
                      "destructors are identified");
        // Each Holder's destructor, by its mangled class (State then int, State then long): one variant
        // where the base and complete destructors are aliases (ELF), both where they are not (Darwin).
        report.expect(count("5StateiED") >= 1 && count("5StatelED") >= 1,
                      "ConstSuggestionImplicitMembers: both Holder destructors are identified");
        report.expect(count("Plain<") == 0,
                      "ConstSuggestionImplicitMembers: a class without a nested type gets none "
                      "(not identified whatever the class)");
        std::map<std::string, std::string> owner;
        bool apart = true;
        for (const cs::UnitFacts::Identity& identity : unit.identities)
        {
            for (const std::string& key :
                 {identity.nested.empty() ? "" : "n" + identity.nested + '\0' + identity.enclosing,
                  identity.specialization.empty() ? "" : "s" + identity.specialization})
            {
                if (key.empty())
                    continue;
                const auto [it, added] = owner.try_emplace(key, identity.symbol);
                apart = apart && (added || it->second == identity.symbol);
            }
        }
        report.expect(apart, "ConstSuggestionImplicitMembers: two symbols never share a key (the "
                             "type, the variant and the class's arguments keep them apart)");
        return report.failures == 0;
    }

    /// #217 (Task 3.2, O1): the correspondences of §5.7 keep the multiplicity and the order of
    /// the references at each key, those with a template and those without alike, and map the
    /// instantiation contexts through the pairs that earlier rounds establish. These cases pin
    /// what an index built once must give.
    bool testConstSuggestionCorrespondenceIndex(TestReport& report)
    {
        namespace cs = ctrace::stack::analysis::const_suggestion;
        const auto site = [](std::size_t offset)
        {
            cs::DeclarationSite out;
            out.expansion = {"use.cpp", offset};
            out.spelling = out.expansion;
            return out;
        };
        const auto reference = [](cs::FactKey key, std::vector<std::string> value)
        {
            cs::Fact fact;
            fact.key = std::move(key);
            fact.value = std::move(value);
            fact.shown = fact.value.front();
            return fact;
        };
        const cs::FactKey at{{"ctx"}, site(10)};
        const cs::Fact withTemplate = reference(at, {"symbol:_Z1aIiEvv", "template:a"});
        const cs::Fact rewritten = reference(at, {"symbol:_Z1aIKiEvv", "template:a"});
        const cs::Fact plain = reference(at, {"site:use.cpp:3"});
        const auto pairsOf = [&](std::vector<cs::Fact> before, std::vector<cs::Fact> after,
                                 std::set<std::string> instantiations)
        {
            cs::UnitFacts was;
            was.entities.resize(1);
            was.facts = std::move(before);
            cs::UnitFacts now;
            now.entities.resize(1);
            now.facts = std::move(after);
            now.instantiations = std::move(instantiations);
            std::string missing;
            std::vector<std::string> out;
            for (const ctrace::stack::analysis::SymbolPair& pair :
                 cs::correspond(was, 0, now, missing))
                out.push_back(pair.before + "->" + pair.after);
            return out;
        };
        using Pairs = std::vector<std::string>;
        report.expect(pairsOf({withTemplate, plain}, {rewritten}, {"ctx"}).empty(),
                      "ConstSuggestionCorrespondence: two references before, one after, at one "
                      "key: no pair, though only one has a template");
        report.expect(pairsOf({withTemplate, plain}, {rewritten, plain}, {"ctx"}) ==
                          Pairs{"_Z1aIiEvv->_Z1aIKiEvv"},
                      "ConstSuggestionCorrespondence: as many on both sides, in the same order: "
                      "the template's references pair");
        report.expect(pairsOf({withTemplate, plain}, {plain, rewritten}, {"ctx"}).empty(),
                      "ConstSuggestionCorrespondence: as many on both sides, in another order: "
                      "no pair");
        const cs::Fact outer = reference({{}, site(20)}, {"symbol:_Z1xIiEvv", "template:x"});
        const cs::Fact outerAfter = reference({{}, site(20)}, {"symbol:_Z1xIKiEvv", "template:x"});
        const cs::Fact inner =
            reference({{"_Z1xIiEvv"}, site(30)}, {"symbol:_Z1yIiEvv", "template:y"});
        const cs::Fact innerAfter =
            reference({{"_Z1xIKiEvv"}, site(30)}, {"symbol:_Z1yIKiEvv", "template:y"});
        report.expect(pairsOf({outer, inner}, {outerAfter, innerAfter}, {}) ==
                          Pairs{"_Z1xIiEvv->_Z1xIKiEvv", "_Z1yIiEvv->_Z1yIKiEvv"},
                      "ConstSuggestionCorrespondence: a context maps through the pair an earlier "
                      "round establishes");
        return report.failures == 0;
    }

    bool testUninitializedSummaryKeysAreLinkerSymbols(const std::filesystem::path& repoRoot,
                                                      TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/uninit_fixpoint_budget_input.c", config,
                                  loaded, loadError))
        {
            report.expect(false, "UninitSummaryKeys setup: failed to load module: " + loadError);
            return false;
        }
        const UninitializedSummaryIndex index = buildUninitializedSummaryIndex(
            *loaded.module, [](const llvm::Function&) { return true; },
            static_cast<const UninitializedSummaryIndex*>(nullptr));
        const std::string key = linkerSymbolName(*loaded.module->getFunction("fill"));
        report.expect(index.functions.count(key) == 1,
                      "UninitSummaryKeys: the summary of fill is keyed by its linker symbol");
        return report.failures == 0;
    }

    // Three modules (#157): A is built with a one-iteration limit, at which none of its analyses
    // converges; B and C with the normal budget, B with the index of A, C with those of A and B.
    bool testUninitializedIncompleteSummaries(const std::filesystem::path& repoRoot,
                                              TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule a;
        LoadedModule b;
        LoadedModule c;
        std::string loadError;
        const std::pair<LoadedModule*, const char*> inputs[] = {{&a, "a"}, {&b, "b"}, {&c, "c"}};
        for (const auto& [module, file] : inputs)
        {
            const std::filesystem::path source =
                repoRoot / "test/unit" / (std::string("uninit_incomplete_") + file + ".c");
            if (!loadModuleFromSource(source, config, *module, loadError))
            {
                report.expect(false, "UninitIncomplete setup: failed to load module: " + loadError);
                return false;
            }
        }

        auto analyzeAll = [](const llvm::Function&) { return true; };
        // Looked up by linker symbol, which is portable: "f" on Linux, "_f" on macOS.
        const auto entry = [](const UninitializedSummaryIndex& index, const llvm::Module& module,
                              const char* name) -> const UninitializedSummaryFunction*
        {
            const auto it = index.functions.find(linkerSymbolName(*module.getFunction(name)));
            return it == index.functions.end() ? nullptr : &it->second;
        };
        const auto reads = [](const std::vector<UninitializedLocalReadIssue>& issues)
        {
            std::set<std::string> names;
            for (const UninitializedLocalReadIssue& issue : issues)
                if (issue.kind == UninitializedLocalIssueKind::ReadBeforeDefiniteInit)
                    names.insert(issue.funcName);
            return names;
        };

        const UninitializedSummaryIndex indexA = buildUninitializedSummaryIndex(
            *a.module, analyzeAll, static_cast<const UninitializedSummaryIndex*>(nullptr),
            /*fixpointIterationLimit=*/1);
        for (const char* name : {"f", "fi"})
        {
            const UninitializedSummaryFunction* summary = entry(indexA, *a.module, name);
            report.expect(summary && !summary->complete,
                          std::string("UninitIncomplete: ") + name + " is exported, incomplete");
        }

        const UninitializedSummaryIndex indexB =
            buildUninitializedSummaryIndex(*b.module, analyzeAll, &indexA);
        for (const char* name : {"gv", "gv2"})
        {
            const UninitializedSummaryFunction* summary = entry(indexB, *b.module, name);
            report.expect(summary && !summary->complete,
                          std::string("UninitIncomplete: ") + name +
                              " is incomplete through an intermediate function");
        }
        const UninitializedSummaryFunction* h = entry(indexB, *b.module, "h");
        report.expect(h && h->complete, "UninitIncomplete: h is complete, its empty summary kept");

        const std::set<std::string> readsB =
            reads(analyzeUninitializedLocalReads(*b.module, analyzeAll, &indexA));
        report.expect(readsB ==
                          reads(analyzeUninitializedLocalReads(*b.module, analyzeAll, nullptr)),
                      "UninitIncomplete: B reports as if no summary were imported");
        report.expect(readsB.count("use_fi") == 1 && readsB.count("use_f") == 0,
                      "UninitIncomplete: int fi() keeps the report of an unknown declaration");

        UninitializedSummaryIndex indexAB = indexA;
        (void)mergeUninitializedSummaryIndex(indexAB, indexB);
        const std::set<std::string> readsC =
            reads(analyzeUninitializedLocalReads(*c.module, analyzeAll, &indexAB));
        report.expect(readsC ==
                          reads(analyzeUninitializedLocalReads(*c.module, analyzeAll, nullptr)),
                      "UninitIncomplete: C reports as if no summary were imported");
        report.expect(readsC.count("use_gv2") == 0,
                      "UninitIncomplete: no caller reads the empty summary of gv2 as no write");
        return report.failures == 0;
    }

    // #168: in one file too, the summary of a dataflow stopped before its fixpoint is absent
    // for its effects (spec §4.1): a call to it reports what a call to a declaration reports.
    bool testUninitializedIncompleteSummariesInOneFile(const std::filesystem::path& repoRoot,
                                                       TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/uninit_incomplete_same_file.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false,
                          "UninitIncompleteOneFile setup: failed to load module: " + loadError);
            return false;
        }

        auto analyzeAll = [](const llvm::Function&) { return true; };
        using Kind = UninitializedLocalIssueKind;
        // The issues of one function, as (kind, variable) pairs.
        const auto issuesOf =
            [](const std::vector<UninitializedLocalReadIssue>& issues, const char* func)
        {
            std::multiset<std::pair<Kind, std::string>> out;
            for (const UninitializedLocalReadIssue& issue : issues)
                if (issue.funcName == func)
                    out.emplace(issue.kind, issue.varName);
            return out;
        };
        const auto has = [](const std::multiset<std::pair<Kind, std::string>>& issues, Kind kind)
        {
            return std::any_of(issues.begin(), issues.end(),
                               [kind](const auto& issue) { return issue.first == kind; });
        };

        // Automatic budget: both callees converge, and their summaries are exact.
        const std::vector<UninitializedLocalReadIssue> converged =
            analyzeUninitializedLocalReads(*loaded.module, analyzeAll, nullptr);
        report.expect(issuesOf(converged, "write_then_read").empty(),
                      "UninitIncompleteOneFile: converged produce initializes x");
        report.expect(issuesOf(converged, "pass_to_reader")
                              .count({Kind::ReadBeforeDefiniteInitViaCall, "x"}) == 1,
                      "UninitIncompleteOneFile: converged consume reads x");

        // Budget of two iterations: the callees stop before their fixpoint, their callers do not.
        const std::vector<UninitializedLocalReadIssue> stopped =
            analyzeUninitializedLocalReads(*loaded.module, analyzeAll, nullptr,
                                           /*fixpointIterationLimit=*/2);
        report.expect(has(issuesOf(stopped, "produce"), Kind::AnalysisIncomplete) &&
                          has(issuesOf(stopped, "consume"), Kind::AnalysisIncomplete),
                      "UninitIncompleteOneFile: the budget stops produce and consume");
        report.expect(issuesOf(stopped, "write_then_read") ==
                          issuesOf(stopped, "write_then_read_decl"),
                      "UninitIncompleteOneFile: a call to stopped produce reports as a call to a "
                      "declaration");
        report.expect(issuesOf(stopped, "pass_to_reader") ==
                          issuesOf(stopped, "pass_to_reader_decl"),
                      "UninitIncompleteOneFile: a call to stopped consume reports as a call to a "
                      "declaration");

        // Looked up by linker symbol, which is portable: "f" on Linux, "_f" on macOS.
        const UninitializedSummaryIndex index = buildUninitializedSummaryIndex(
            *loaded.module, analyzeAll, static_cast<const UninitializedSummaryIndex*>(nullptr),
            /*fixpointIterationLimit=*/2);
        const auto complete = [&](const char* name) -> std::optional<bool>
        {
            const auto it =
                index.functions.find(linkerSymbolName(*loaded.module->getFunction(name)));
            if (it == index.functions.end())
                return std::nullopt;
            return static_cast<bool>(it->second.complete);
        };
        report.expect(complete("write_then_read") == false && complete("pass_to_reader") == false,
                      "UninitIncompleteOneFile: a caller of a stopped callee is incomplete");
        report.expect(complete("write_then_read_decl") == true &&
                          complete("pass_to_reader_decl") == true,
                      "UninitIncompleteOneFile: a caller of a declaration stays complete");
        return report.failures == 0;
    }

    // A cyclic group stopped at its iteration cap (#157): the driver must mark its summaries
    // incomplete, and must not when the group converges.
    bool testCrossTUDriverMarksUnconvergedCycles(TestReport& report)
    {
        using ctrace::stack::app::detail::CrossTUSummaryPlan;
        using ctrace::stack::app::detail::runCrossTUSummaryPass;

        struct Entry
        {
            int value = 0;
            bool complete = true;
        };
        struct Operations
        {
            using Index = std::map<std::string, Entry>;
            using External = int;
            bool converges = false;
            mutable int round = 0;
            int marked = 0;
            External prepareLevel(const Index&) const
            {
                return 0;
            }
            External prepareCycle(const Index&) const
            {
                return 0;
            }
            Index build(std::size_t module, const Index&, const External&) const
            {
                return {{module == 0 ? "a" : "b", {converges ? 1 : ++round, true}}};
            }
            bool tryCache(std::size_t, const External&, const Index&) const
            {
                return false;
            }
            void cache(std::size_t, const External&, const Index&) const {}
            static void merge(Index& into, const Index& from)
            {
                for (const auto& [name, value] : from)
                {
                    auto [it, inserted] = into.emplace(name, value);
                    if (!inserted)
                        it->second = {value.value, it->second.complete && value.complete};
                }
            }
            static bool equals(const Index& lhs, const Index& rhs)
            {
                return lhs.size() == rhs.size() &&
                       std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                                  [](const auto& x, const auto& y)
                                  {
                                      return x.first == y.first &&
                                             x.second.value == y.second.value &&
                                             x.second.complete == y.second.complete;
                                  });
            }
            static std::unordered_set<std::string> changedNames(const Index&, const Index& next)
            {
                std::unordered_set<std::string> names;
                for (const auto& item : next)
                    names.insert(item.first);
                return names;
            }
            void markIncomplete(Index& index)
            {
                ++marked;
                for (auto& item : index)
                    item.second.complete = false;
            }
            void reportIteration(std::size_t, unsigned, bool, std::size_t) const {}
            void reportLimit(std::size_t, unsigned) const {}
            void reportLevel(std::size_t, const CrossTUSummaryPlan::Level&, std::int64_t) const {}
        };

        // Two modules that call each other: one cyclic group.
        const std::vector<std::unordered_set<std::string>> callees = {{"b"}, {"a"}};
        const std::unordered_map<std::string, std::vector<std::size_t>> definitions = {{"a", {0}},
                                                                                       {"b", {1}}};
        const CrossTUSummaryPlan plan(callees, definitions);
        const auto serial = [](const std::vector<std::size_t>& modules, const auto& build)
        {
            for (std::size_t module : modules)
                build(module);
        };
        for (const bool converges : {false, true})
        {
            Operations operations;
            operations.converges = converges;
            Operations::Index global;
            std::vector<Operations::Index> moduleSummaries(2);
            (void)runCrossTUSummaryPass(plan, global, moduleSummaries, operations, serial);
            const bool both = global.size() == 2;
            if (converges)
                report.expect(operations.marked == 0 && both && global.at("a").complete &&
                                  global.at("b").complete,
                              "CrossTUDriver: a converged cycle keeps its summaries complete");
            else
                report.expect(operations.marked == 2 && both && !global.at("a").complete &&
                                  !global.at("b").complete,
                              "CrossTUDriver: a cycle stopped at its cap marks its summaries "
                              "incomplete");
        }
        return report.failures == 0;
    }

    bool testProgramPointRanges(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;

        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/int_range_point_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "ProgramPointRanges setup: failed to load module: " + loadError);
            return false;
        }

        // The array accesses are the GEPs; the index key is the slot the index was loaded
        // from (-O0 keeps `i` in an alloca and reloads it before every use).
        struct Access
        {
            const llvm::Instruction* inst = nullptr;
            const llvm::Value* key = nullptr;
        };
        const auto accessesOf = [&](const char* name) -> std::vector<Access>
        {
            std::vector<Access> out;
            llvm::Function* fn = loaded.module->getFunction(name);
            if (!fn)
                return out;
            for (llvm::BasicBlock& block : *fn)
            {
                for (llvm::Instruction& instruction : block)
                {
                    const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
                    if (!gep || gep->getNumIndices() == 0)
                        continue;
                    const llvm::Value* index = gep->getOperand(gep->getNumOperands() - 1);
                    if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(index))
                        index = cast->getOperand(0);
                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(index);
                    if (!load)
                        continue;
                    out.push_back({gep, load->getPointerOperand()});
                }
            }
            return out;
        };

        {
            llvm::Function* fn = loaded.module->getFunction("early_return_guard");
            const std::vector<Access> accesses = accessesOf("early_return_guard");
            report.expect(fn != nullptr && accesses.size() == 1,
                          "ProgramPointRanges: early_return_guard has one array access");
            if (fn && accesses.size() == 1)
            {
                const FunctionFacts facts(*fn);
                const ProgramPointRanges ranges(*fn, facts);
                const std::optional<IntRange> r = ranges.at(accesses[0].key, *accesses[0].inst);
                report.expect(r && r->hasUpper && r->upper == 199,
                              "ProgramPointRanges: false edge of `i >= 200` gives i <= 199");
                report.expect(!(r && r->hasLower && r->lower >= 200),
                              "ProgramPointRanges: true edge of `i >= 200` does not leak past "
                              "the return");
            }
        }

        {
            llvm::Function* fn = loaded.module->getFunction("else_branch");
            const std::vector<Access> accesses = accessesOf("else_branch");
            report.expect(fn != nullptr && accesses.size() == 2,
                          "ProgramPointRanges: else_branch has two array accesses");
            if (fn && accesses.size() == 2)
            {
                const FunctionFacts facts(*fn);
                const ProgramPointRanges ranges(*fn, facts);
                const std::optional<IntRange> thenRange =
                    ranges.at(accesses[0].key, *accesses[0].inst);
                const std::optional<IntRange> elseRange =
                    ranges.at(accesses[1].key, *accesses[1].inst);
                report.expect(thenRange && thenRange->hasUpper && thenRange->upper == 9,
                              "ProgramPointRanges: then-block of `i <= 9` knows i <= 9");
                report.expect(elseRange && elseRange->hasLower && elseRange->lower == 10,
                              "ProgramPointRanges: else-block of `i <= 9` knows i >= 10");
                report.expect(!(elseRange && elseRange->hasUpper),
                              "ProgramPointRanges: then-constraint does not leak into the else");
            }
        }

        {
            llvm::Function* fn = loaded.module->getFunction("merge_after_guard");
            const std::vector<Access> accesses = accessesOf("merge_after_guard");
            report.expect(fn != nullptr && accesses.size() == 1,
                          "ProgramPointRanges: merge_after_guard has one array access");
            if (fn && accesses.size() == 1)
            {
                const FunctionFacts facts(*fn);
                const ProgramPointRanges ranges(*fn, facts);
                const std::optional<IntRange> r = ranges.at(accesses[0].key, *accesses[0].inst);
                report.expect(!(r && (r->hasUpper || r->hasLower)),
                              "ProgramPointRanges: a non-dominating guard bounds nothing at the "
                              "merge");

                // The whole-map view used by the SMT encoder agrees with the point query.
                const std::map<const llvm::Value*, IntRange> snapshot =
                    ranges.at(*accesses[0].inst);
                report.expect(snapshot.count(accesses[0].key) == 0,
                              "ProgramPointRanges: snapshot at the merge carries no bound for i");
            }
        }

        {
            llvm::Function* fn = loaded.module->getFunction("reused_loop_variable");
            const std::vector<Access> accesses = accessesOf("reused_loop_variable");
            // a[i] in loop 1, b[i] and a[i] in loop 2, b[3] is a constant index (no load).
            report.expect(fn != nullptr && accesses.size() == 3,
                          "ProgramPointRanges: reused_loop_variable has three indexed accesses");
            if (fn && accesses.size() == 3)
            {
                const FunctionFacts facts(*fn);
                const ProgramPointRanges ranges(*fn, facts);
                const std::optional<IntRange> first = ranges.at(accesses[0].key, *accesses[0].inst);
                const std::optional<IntRange> second =
                    ranges.at(accesses[1].key, *accesses[1].inst);
                report.expect(first && first->hasUpper && first->upper == 16,
                              "ProgramPointRanges: a loop guard bounds its own body despite the "
                              "increment");
                report.expect(second && second->hasUpper && second->upper == 15,
                              "ProgramPointRanges: the second loop's guard bounds its body");
                report.expect(!(second && second->hasLower && second->lower >= 17),
                              "ProgramPointRanges: the first loop's exit edge is killed by the "
                              "rewrite of the slot");
            }
        }

        return report.failures == 0;
    }
    bool testResourceModelConditions(TestReport& report)
    {
        using namespace ctrace::stack::analysis;

        const auto parseText = [](const std::string& text, ResourceModel& model, std::string& error)
        {
            char path[] = "/tmp/ctrace-model-XXXXXX";
            const int fd = mkstemp(path);
            if (fd < 0)
                return false;
            (void)write(fd, text.data(), text.size());
            close(fd);
            const bool ok = parseResourceModel(path, model, error);
            unlink(path);
            return ok;
        };

        ResourceModel model;
        std::string error;
        const bool ok = parseText("acquire_out f 0 K if_ret==0\n"
                                  "acquire_ret g K if_ret!=null\n"
                                  "release_arg h 0 K\n"
                                  "release_arg i 1 K if_ret>=0\n",
                                  model, error);
        report.expect(ok && model.rules.size() == 4,
                      "ResourceModel: qualified and unqualified rules parse: " + error);
        if (ok && model.rules.size() == 4)
        {
            report.expect(model.rules[0].condition == RuleCondition::RetEqZero,
                          "ResourceModel: if_ret==0");
            report.expect(model.rules[1].condition == RuleCondition::RetNeNull,
                          "ResourceModel: if_ret!=null");
            report.expect(model.rules[2].condition == RuleCondition::Always,
                          "ResourceModel: no qualifier means Always");
            report.expect(model.rules[3].condition == RuleCondition::RetGeZero &&
                              model.rules[3].argIndex == 1,
                          "ResourceModel: if_ret>=0 keeps the argument index");
        }

        ResourceModel bad;
        std::string badError;
        const bool badOk = parseText("acquire_out f 0 K if_ret=0\n", bad, badError);
        report.expect(!badOk && badError.find("line 1") != std::string::npos,
                      "ResourceModel: an unknown qualifier is an error naming the line");
        return report.failures == 0;
    }
    bool testOwnershipFactCollector(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        using namespace ctrace::stack::analysis::ownership;
        using Kind = Event::Kind;

        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/ownership_collector_input.c";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "OwnershipCollector setup: failed to load module: " + loadError);
            return false;
        }
        ResourceModel model;
        std::string modelError;
        if (!parseResourceModel((repoRoot / "test/unit/ownership_collector_model.txt").string(),
                                model, modelError))
        {
            report.expect(false, "OwnershipCollector setup: model: " + modelError);
            return false;
        }
        const SummaryLookup noSummaries{[](const llvm::Function&) { return nullptr; }};

        // Kinds of all block events in block order, then edge events, as a flat list.
        const auto kindsOf = [&](const char* name, std::vector<Kind>& blockKinds,
                                 std::vector<Kind>& edgeKinds) -> bool
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            if (!fn)
                return false;
            const CollectedFunction c =
                collectOwnershipFacts(*fn, model, noSummaries, loaded.module->getDataLayout());
            for (const Block& b : c.facts.blocks)
                for (const Event& e : b.events)
                    blockKinds.push_back(e.kind);
            for (const Edge& e : c.facts.edges)
                for (const Event& ev : e.events)
                {
                    blockKinds.push_back(ev.kind);
                    edgeKinds.push_back(ev.kind);
                }
            return true;
        };
        const auto count = [](const std::vector<Kind>& v, Kind k)
        { return std::count(v.begin(), v.end(), k); };

        {
            std::vector<Kind> blocks, edges;
            report.expect(kindsOf("early_return", blocks, edges),
                          "OwnershipCollector: early_return collected");
            report.expect(count(blocks, Kind::Acquire) == 1 && count(blocks, Kind::Release) == 1,
                          "OwnershipCollector: early_return has one acquire and one release");
            report.expect(count(blocks, Kind::Exit) == 2 && count(blocks, Kind::Return) == 0,
                          "OwnershipCollector: the merged int ret is split into one exit per "
                          "incoming path, with no handle return");
            report.expect(
                count(blocks, Kind::UnknownCall) == 0 && count(blocks, Kind::AddressEscape) == 0,
                "OwnershipCollector: check() without pointer args is not an unknown call");
        }
        {
            std::vector<Kind> blocks, edges;
            report.expect(kindsOf("alias_keep", blocks, edges),
                          "OwnershipCollector: alias_keep collected");
            report.expect(count(blocks, Kind::Acquire) == 2 && count(blocks, Kind::Release) == 2,
                          "OwnershipCollector: alias_keep has two acquires and two releases");
            report.expect(count(blocks, Kind::Copy) >= 1,
                          "OwnershipCollector: saved = h is a copy between locations");
        }
        {
            llvm::Function* fn = loaded.module->getFunction("select_return");
            report.expect(fn != nullptr, "OwnershipCollector: select_return exists");
            if (fn)
            {
                const CollectedFunction c =
                    collectOwnershipFacts(*fn, model, noSummaries, loaded.module->getDataLayout());
                // `c ? h : 0` is a phi at -O0: one incoming edge copies h, the other
                // writes null; both target the phi's location so the join keeps both.
                std::optional<LocationId> copyDst;
                std::optional<LocationId> nullDst;
                for (const Edge& e : c.facts.edges)
                    for (const Event& ev : e.events)
                    {
                        if (ev.kind == Event::Kind::Copy)
                            copyDst = ev.dst;
                        if (ev.kind == Event::Kind::Overwrite && !ev.unknownValue)
                            nullDst = ev.dst;
                    }
                report.expect(copyDst && nullDst && *copyDst == *nullDst,
                              "OwnershipCollector: phi keeps both alternatives on its edges");
                report.expect(c.facts.siteCount == 1 && c.siteInstructions.size() == 1,
                              "OwnershipCollector: acquire_ret is one acquisition site");
            }
        }
        {
            std::vector<Kind> blocks, edges;
            report.expect(kindsOf("unknown_call", blocks, edges),
                          "OwnershipCollector: unknown_call collected");
            report.expect(count(blocks, Kind::UnknownCall) == 1,
                          "OwnershipCollector: passing a handle to an unmodelled callee");
            report.expect(count(blocks, Kind::AddressEscape) == 1,
                          "OwnershipCollector: passing &h to an unmodelled callee");
        }
        return report.failures == 0;
    }
    bool testOwnershipCollectorExceptions(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        using namespace ctrace::stack::analysis::ownership;
        using Kind = Event::Kind;

        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        const std::filesystem::path source = repoRoot / "test/unit/ownership_collector_input.cpp";
        if (!loadModuleFromSource(source, config, loaded, loadError))
        {
            report.expect(false, "OwnershipCollectorExceptions setup: failed to load module: " +
                                     loadError);
            return false;
        }
        ResourceModel model;
        std::string modelError;
        (void)parseResourceModel((repoRoot / "test/unit/ownership_collector_model.txt").string(),
                                 model, modelError);
        const SummaryLookup noSummaries{[](const llvm::Function&) { return nullptr; }};

        const auto collect = [&](const char* mangledPrefix) -> std::optional<CollectedFunction>
        {
            for (llvm::Function& fn : *loaded.module)
            {
                if (fn.isDeclaration() || !fn.getName().starts_with(mangledPrefix))
                    continue;
                return collectOwnershipFacts(fn, model, noSummaries,
                                             loaded.module->getDataLayout());
            }
            return std::nullopt;
        };
        // Position of the first event of `kind`, blocks then edges, or -1.
        const auto firstIndex = [](const CollectedFunction& c, Kind kind, bool exceptional)
        {
            int index = 0;
            const auto scan = [&](const std::vector<Event>& events)
            {
                for (const Event& e : events)
                {
                    if (e.kind == kind && (kind != Kind::Exit || e.exceptional == exceptional))
                        return true;
                    ++index;
                }
                return false;
            };
            for (const Block& b : c.facts.blocks)
                if (scan(b.events))
                    return index;
            for (const Edge& e : c.facts.edges)
                if (scan(e.events))
                    return index;
            return -1;
        };

        {
            const auto c = collect("_Z14call_may_throwv");
            report.expect(c.has_value(), "OwnershipCollectorExceptions: call_may_throw collected");
            if (c)
            {
                const int exceptionalExit = firstIndex(*c, Kind::Exit, true);
                const int release = firstIndex(*c, Kind::Release, false);
                report.expect(exceptionalExit >= 0 && release >= 0 && exceptionalExit < release,
                              "OwnershipCollectorExceptions: a may-throw call is an exceptional "
                              "exit before the release");
            }
        }
        {
            const auto c = collect("_Z12call_nothrowv");
            report.expect(c.has_value(), "OwnershipCollectorExceptions: call_nothrow collected");
            if (c)
                report.expect(firstIndex(*c, Kind::Exit, true) < 0,
                              "OwnershipCollectorExceptions: a noexcept function has no "
                              "exceptional exit");
        }
        {
            const auto c = collect("_Z17invoke_with_catchv");
            report.expect(c.has_value(),
                          "OwnershipCollectorExceptions: invoke_with_catch collected");
            if (c)
            {
                // The caught invoke's unwind edge lands in this function: it carries the
                // callee's weakened effects, never an exit of this function.
                const llvm::BasicBlock* unwindFrom = nullptr;
                const llvm::BasicBlock* unwindTo = nullptr;
                for (const llvm::BasicBlock* bb : c->blockOf)
                    for (const llvm::Instruction& I : *bb)
                        if (const auto* inv = llvm::dyn_cast<llvm::InvokeInst>(&I))
                        {
                            unwindFrom = inv->getParent();
                            unwindTo = inv->getUnwindDest();
                        }
                bool exitOnUnwindEdge = false;
                for (const Edge& edge : c->facts.edges)
                {
                    if (c->blockOf[edge.from] != unwindFrom || c->blockOf[edge.to] != unwindTo)
                        continue;
                    for (const Event& ev : edge.events)
                        exitOnUnwindEdge = exitOnUnwindEdge || ev.kind == Kind::Exit;
                }
                report.expect(unwindFrom != nullptr && !exitOnUnwindEdge,
                              "OwnershipCollectorExceptions: the invoke's unwind edge is not "
                              "an exit of this function");
                report.expect(firstIndex(*c, Kind::Release, false) >= 0,
                              "OwnershipCollectorExceptions: the release after the catch is seen");
            }
        }
        return report.failures == 0;
    }
    bool testOwnershipSummaryIndex(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        using namespace ctrace::stack::analysis::ownership;

        const ctrace::stack::AnalysisConfig config;
        const auto owned = static_cast<std::size_t>(OwnState::Owned);
        const std::string model = (repoRoot / "models/resource-lifetime/generic.txt").string();
        const auto all = [](const llvm::Function&) { return true; };

        {
            LoadedModule loaded;
            std::string loadError;
            const std::filesystem::path source =
                repoRoot / "test/resource-lifetime/cross-tu-release-sometimes-def.c";
            if (!loadModuleFromSource(source, config, loaded, loadError))
            {
                report.expect(false, "OwnershipSummaryIndex setup: " + loadError);
                return false;
            }
            const ResourceSummaryIndex index =
                buildResourceLifetimeSummaryIndex(*loaded.module, all, model, nullptr);
            const auto it = index.functions.find("release_sometimes_cross_tu");
            const bool ok = it != index.functions.end() && it->second.ownership.normal.present &&
                            it->second.ownership.normal.params.count(0) == 1;
            report.expect(ok, "OwnershipSummaryIndex: the exported index carries a transformer");
            if (ok)
            {
                const StateSet img = it->second.ownership.normal.params.at(0)[owned];
                report.expect(img.has(OwnState::Owned) && img.has(OwnState::Released),
                              "OwnershipSummaryIndex: release-sometimes maps Owned to "
                              "{Owned, Released}");
            }
        }
        {
            LoadedModule loaded;
            std::string loadError;
            const std::filesystem::path source =
                repoRoot / "test/resource-lifetime/cross-tu-release-always-def.c";
            if (!loadModuleFromSource(source, config, loaded, loadError))
            {
                report.expect(false, "OwnershipSummaryIndex setup: " + loadError);
                return false;
            }
            const ResourceSummaryIndex index =
                buildResourceLifetimeSummaryIndex(*loaded.module, all, model, nullptr);
            const auto it = index.functions.find("release_always_cross_tu");
            report.expect(
                it != index.functions.end() && it->second.ownership.normal.params.count(0) == 1 &&
                    it->second.ownership.normal.params.at(0)[owned].isOnly(OwnState::Released),
                "OwnershipSummaryIndex: release-always maps Owned to {Released}");
        }
        {
            // The legacy out-param wrapper (props->out) is exported as a viaPointerSlot path.
            LoadedModule loaded;
            std::string loadError;
            const std::filesystem::path source =
                repoRoot / "test/resource-lifetime/cross-tu-wrapper-def.c";
            if (!loadModuleFromSource(source, config, loaded, loadError))
            {
                report.expect(false, "OwnershipSummaryIndex setup: " + loadError);
                return false;
            }
            const ResourceSummaryIndex index =
                buildResourceLifetimeSummaryIndex(*loaded.module, all, model, nullptr);
            const auto it = index.functions.find("create_wrapper_cross_tu");
            bool viaSlot = false;
            if (it != index.functions.end())
                for (const auto& [path, fresh] : it->second.ownership.normal.outArgs)
                    viaSlot = viaSlot || (path.argIndex == 0 && path.viaPointerSlot &&
                                          fresh.certainty == Certainty::Guaranteed &&
                                          fresh.kind == "GenericHandle");
            report.expect(viaSlot, "OwnershipSummaryIndex: acquisition through props->out is a "
                                   "guaranteed viaPointerSlot out-arg");
        }
        return report.failures == 0;
    }

    /// Loads of @p fn whose pointer operand is named @p slot, in instruction order.
    std::vector<const llvm::LoadInst*> loadsFrom(const llvm::Function* fn, llvm::StringRef slot)
    {
        std::vector<const llvm::LoadInst*> out;
        if (!fn)
            return out;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
            if (load && load->getPointerOperand()->getName() == slot)
                out.push_back(load);
        }
        return out;
    }

    /// FunctionFacts::clobberingAccess: MemorySSA clobbers of -O0 slot reads.
    bool testFunctionFactsClobberingAccess(const std::filesystem::path& repoRoot,
                                           TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "clobberingAccess setup: failed to load module: " + loadError);
            return false;
        }

        {
            llvm::Function* fn = loaded.module->getFunction("reads_param_twice");
            const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "x.addr");
            report.expect(loads.size() == 2, "clobberingAccess: reads_param_twice reads x twice");
            if (loads.size() == 2)
            {
                const FunctionFacts facts(*fn);
                const llvm::MemoryAccess* first = facts.clobberingAccess(*loads[0]);
                report.expect(first != nullptr && first == facts.clobberingAccess(*loads[1]),
                              "clobberingAccess: two reads with no write between share it");
                const auto* def = llvm::dyn_cast_or_null<llvm::MemoryDef>(first);
                const auto* store =
                    def ? llvm::dyn_cast_or_null<llvm::StoreInst>(def->getMemoryInst()) : nullptr;
                report.expect(store && llvm::isa<llvm::Argument>(store->getValueOperand()),
                              "clobberingAccess: it is the store of the parameter");
            }
        }

        {
            llvm::Function* fn = loaded.module->getFunction("reads_across_calls");
            const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "x");
            report.expect(loads.size() == 2, "clobberingAccess: reads_across_calls reads x twice");
            if (loads.size() == 2)
            {
                const FunctionFacts facts(*fn);
                const auto* first =
                    llvm::dyn_cast_or_null<llvm::MemoryDef>(facts.clobberingAccess(*loads[0]));
                const auto* second =
                    llvm::dyn_cast_or_null<llvm::MemoryDef>(facts.clobberingAccess(*loads[1]));
                report.expect(first && second && first != second &&
                                  llvm::isa_and_nonnull<llvm::CallBase>(first->getMemoryInst()) &&
                                  llvm::isa_and_nonnull<llvm::CallBase>(second->getMemoryInst()),
                              "clobberingAccess: a call that may write the slot separates reads");
            }
        }
        return report.failures == 0;
    }

    using ctrace::stack::analysis::smt::ConstraintIR;
    using ctrace::stack::analysis::smt::ExprId;
    using ctrace::stack::analysis::smt::ExprKind;
    using ctrace::stack::analysis::smt::ExprNode;
    using ctrace::stack::analysis::smt::QueryPoint;

    /// First binary operator of @p fn with @p opcode, or nullptr.
    const llvm::BinaryOperator* firstBinary(const llvm::Function* fn, unsigned opcode)
    {
        if (!fn)
            return nullptr;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
            if (binary && binary->getOpcode() == opcode)
                return binary;
        }
        return nullptr;
    }

    /// Last binary operator of @p fn with @p opcode, or nullptr.
    const llvm::BinaryOperator* lastBinary(const llvm::Function* fn, unsigned opcode)
    {
        const llvm::BinaryOperator* last = nullptr;
        if (!fn)
            return last;
        for (const llvm::Instruction& inst : llvm::instructions(*fn))
        {
            const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst);
            if (binary && binary->getOpcode() == opcode)
                last = binary;
        }
        return last;
    }

    /// First node of @p ir with @p kind, or nullptr.
    const ExprNode* firstNode(const ConstraintIR& ir, ExprKind kind)
    {
        const auto it = std::find_if(ir.nodes.begin(), ir.nodes.end(),
                                     [&](const ExprNode& node) { return node.kind == kind; });
        return it == ir.nodes.end() ? nullptr : &*it;
    }

    /// Left operand of the first @p kind node whose right operand is the constant @p value.
    std::optional<ExprId> lhsAgainstConstant(const ConstraintIR& ir, ExprKind kind,
                                             std::int64_t value)
    {
        for (const ExprNode& node : ir.nodes)
        {
            if (node.kind != kind)
                continue;
            const ExprNode& rhs = ir.nodes.at(node.rhs);
            if (rhs.kind == ExprKind::Constant && rhs.constant == value)
                return node.lhs;
        }
        return std::nullopt;
    }

    /// Whether @p ir negates a @p kind comparison against the constant @p value.
    bool hasNegatedComparison(const ConstraintIR& ir, ExprKind kind, std::int64_t value)
    {
        return std::any_of(ir.nodes.begin(), ir.nodes.end(),
                           [&](const ExprNode& node)
                           {
                               if (node.kind != ExprKind::Not)
                                   return false;
                               const ExprNode& operand = ir.nodes.at(node.lhs);
                               if (operand.kind != kind)
                                   return false;
                               const ExprNode& rhs = ir.nodes.at(operand.rhs);
                               return rhs.kind == ExprKind::Constant && rhs.constant == value;
                           });
    }

    /// SMT encoder: loads are encoded through their MemorySSA clobber.
    bool testSmtEncoderMemoryModel(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "SMT memory model setup: failed to load module: " + loadError);
            return false;
        }

        // The query on `lhs - rhs` rebuilds the `sub` from its encoded operands, so the first
        // Sub node of the IR says how each operand was encoded.
        const auto subOperands = [&](const char* name,
                                     bool withFacts) -> std::optional<std::pair<ExprId, ExprId>>
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            if (!sub)
                return std::nullopt;
            const FunctionFacts facts(*fn);
            const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                {}, *sub, QueryPoint{.inst = sub, .facts = withFacts ? &facts : nullptr});
            const ExprNode* node = firstNode(ir, ExprKind::Sub);
            if (!node)
                return std::nullopt;
            return std::make_pair(node->lhs, node->rhs);
        };

        {
            const auto with = subOperands("global_read_twice", true);
            const auto without = subOperands("global_read_twice", false);
            report.expect(with && with->first == with->second,
                          "SMT memory model: two reads with no write between are one symbol");
            report.expect(without && without->first != without->second,
                          "SMT memory model: without facts every load stays its own symbol");
        }
        {
            const auto with = subOperands("global_read_across_call", true);
            report.expect(with && with->first != with->second,
                          "SMT memory model: a call that may write the global separates reads");
        }
        {
            llvm::Function* fn = loaded.module->getFunction("forwarded_local");
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            const auto with = subOperands("forwarded_local", true);
            report.expect(with && with->first == with->second,
                          "SMT memory model: a load after a store reads the stored value");
            if (fn && sub)
            {
                const FunctionFacts facts(*fn);
                const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                    {}, *sub, QueryPoint{.inst = sub, .facts = &facts});
                const ExprNode* node = firstNode(ir, ExprKind::Sub);
                const ExprNode* operand = node ? &ir.nodes.at(node->lhs) : nullptr;
                const bool isParameter =
                    operand && operand->kind == ExprKind::Symbol &&
                    std::any_of(
                        ir.symbols.begin(), ir.symbols.end(), [&](const auto& symbol)
                        { return symbol.id == operand->symbol && symbol.debugName == "x"; });
                report.expect(isParameter,
                              "SMT memory model: forwarding reaches the parameter itself");

                // Ranges must bound the very expression the violation uses.
                const std::vector<const llvm::LoadInst*> loads = loadsFrom(fn, "y");
                if (!loads.empty())
                {
                    std::map<const llvm::Value*, IntRange> ranges;
                    ranges[loads[0]] =
                        IntRange{.lower = 0, .upper = 3, .hasLower = true, .hasUpper = true};
                    const ConstraintIR ranged = smt::encodeSignedComparisonFeasibility(
                        ranges, *loads[0], 15, true, QueryPoint{.inst = sub, .facts = &facts});
                    const std::optional<ExprId> bounded =
                        lhsAgainstConstant(ranged, ExprKind::Sle, 3);
                    const std::optional<ExprId> violated =
                        lhsAgainstConstant(ranged, ExprKind::Sgt, 15);
                    report.expect(bounded && violated && *bounded == *violated,
                                  "SMT memory model: a range on a load bounds its encoding");
                }
            }
        }
        {
            // Review focus 1: an i8 store does not define the i32 read that follows it.
            llvm::Function* fn = loaded.module->getFunction("punned_store");
            const llvm::BinaryOperator* sub = firstBinary(fn, llvm::Instruction::Sub);
            if (fn && sub)
            {
                const FunctionFacts facts(*fn);
                const ConstraintIR ir = smt::encodeSignedOverflowFeasibility(
                    {}, *sub, QueryPoint{.inst = sub, .facts = &facts});
                const ExprNode* node = firstNode(ir, ExprKind::Sub);
                report.expect(node && ir.nodes.at(node->lhs).kind == ExprKind::Symbol &&
                                  !lhsAgainstConstant(ir, ExprKind::Eq, 5),
                              "SMT memory model: a narrower store is not forwarded");
            }
            else
            {
                report.expect(false, "SMT memory model: punned_store has a sub");
            }
        }
        {
            // Review focus 2: volatile reads never share a symbol.
            const auto with = subOperands("volatile_read_twice", true);
            report.expect(with && with->first != with->second,
                          "SMT memory model: volatile reads stay distinct");
        }
        return report.failures == 0;
    }

    /// SMT encoder: queries carry the reachability condition of their instruction.
    bool testSmtEncoderPathCondition(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/smt_path_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "SMT path condition setup: failed to load module: " + loadError);
            return false;
        }

        // Overflow query on @p add, with or without the function's facts.
        const auto query = [&](const char* name, bool last, bool withFacts,
                               std::uint64_t budget) -> std::optional<ConstraintIR>
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            const llvm::BinaryOperator* add = last ? lastBinary(fn, llvm::Instruction::Add)
                                                   : firstBinary(fn, llvm::Instruction::Add);
            if (!add)
                return std::nullopt;
            const FunctionFacts facts(*fn);
            return smt::encodeSignedOverflowFeasibility(
                {}, *add,
                QueryPoint{
                    .inst = add, .facts = withFacts ? &facts : nullptr, .budgetNodes = budget});
        };

        {
            const auto with = query("guarded_increment", false, true, 0);
            const auto without = query("guarded_increment", false, false, 0);
            report.expect(with && hasNegatedComparison(*with, ExprKind::Sgt, 5),
                          "SMT path condition: the false edge of `i > 5` guards the add");
            report.expect(without && !hasNegatedComparison(*without, ExprKind::Sgt, 5),
                          "SMT path condition: without facts the query has no path condition");
            const auto tight = query("guarded_increment", false, true, 1);
            report.expect(tight && !hasNegatedComparison(*tight, ExprKind::Sgt, 5),
                          "SMT path condition: a region over budget falls back to no condition");
        }
        {
            const auto with = query("either_positive", false, true, 0);
            report.expect(with && firstNode(*with, ExprKind::Or) != nullptr,
                          "SMT path condition: a block with two predecessors gives a disjunction");
        }
        {
            const auto with = query("irreducible_loop", true, true, 0);
            report.expect(with && !lhsAgainstConstant(*with, ExprKind::Slt, 10) &&
                              !lhsAgainstConstant(*with, ExprKind::Sgt, 0) &&
                              firstNode(*with, ExprKind::Not) == nullptr,
                          "SMT path condition: an irreducible function gets no path condition");
        }
        {
            // Review focus 3: two cases reach the block; the default edge does not.
            const auto with = query("switch_case", false, true, 0);
            report.expect(with && lhsAgainstConstant(*with, ExprKind::Eq, 1) &&
                              lhsAgainstConstant(*with, ExprKind::Eq, 2) &&
                              firstNode(*with, ExprKind::Or) != nullptr,
                          "SMT path condition: switch cases reaching a block are a disjunction");
        }
        {
            // Review focus 4: an instruction of the entry block has no dominator.
            const auto with = query("entry_block_add", false, true, 0);
            report.expect(with && !with->assertions.empty() &&
                              firstNode(*with, ExprKind::Not) == nullptr &&
                              firstNode(*with, ExprKind::Or) == nullptr,
                          "SMT path condition: an entry-block query has no path condition");
        }
        return report.failures == 0;
    }

    /// SMT refinement: an evaluator whose rule has SMT off never builds its query.
    bool testSmtEvaluatorEncodesLazily(TestReport& report)
    {
        struct ProbeEvaluator final : ctrace::stack::analysis::smt::SmtConstraintEvaluator
        {
            using SmtConstraintEvaluator::SmtConstraintEvaluator;

            bool encodes() const
            {
                bool called = false;
                (void)evaluateQuery(
                    [&]
                    {
                        called = true;
                        return ctrace::stack::analysis::smt::ConstraintIR{};
                    });
                return called;
            }
        };

        const ctrace::stack::AnalysisConfig off;
        report.expect(!ProbeEvaluator(off, "stack-buffer").encodes(),
                      "SMT evaluator: SMT off builds no query");

        ctrace::stack::AnalysisConfig otherRule;
        otherRule.smtEnabled = 1;
        otherRule.smtRules = {"recursion"};
        report.expect(!ProbeEvaluator(otherRule, "stack-buffer").encodes(),
                      "SMT evaluator: a rule outside --smt-rules builds no query");

        ctrace::stack::AnalysisConfig on;
        on.smtEnabled = 1;
        report.expect(ProbeEvaluator(on, "stack-buffer").encodes(),
                      "SMT evaluator: SMT on builds the query");
        return report.failures == 0;
    }

#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
    /// Z3 backend: a negative constant wider than 64 bits keeps its sign.
    bool testZ3WideNegativeConstant(TestReport& report)
    {
        using namespace ctrace::stack::analysis::smt;
        ConstraintIR ir;
        ir.symbols.push_back(SymbolInfo{.id = 1, .debugName = "x", .sourceToken = 0});
        const auto add = [&](ExprNode node)
        {
            ir.nodes.push_back(node);
            return static_cast<ExprId>(ir.nodes.size() - 1);
        };
        const ExprId x = add({.kind = ExprKind::Symbol, .symbol = 1, .bitWidth = 128});
        const ExprId minusOne = add({.kind = ExprKind::Constant, .constant = -1, .bitWidth = 128});
        const ExprId zero = add({.kind = ExprKind::Constant, .constant = 0, .bitWidth = 128});
        ir.assertions.push_back(
            add({.kind = ExprKind::Eq, .bitWidth = 1, .lhs = x, .rhs = minusOne}));
        ir.assertions.push_back(add({.kind = ExprKind::Sgt, .bitWidth = 1, .lhs = x, .rhs = zero}));

        SmtQuery query;
        query.ir = std::move(ir);
        query.timeoutMs = 1000;
        const SmtDecision decision =
            SolverOrchestrator(SolverOrchestratorConfig{.primaryBackend = "z3"}).solve(query);
        report.expect(decision.status == SmtStatus::Unsat,
                      "Z3 backend: -1 at 128 bits is negative (x == -1 && x > 0 is unsat)");
        return report.failures == 0;
    }

    /// Z3 backend: two symbols are two solver variables, even when they print the same. The
    /// memory model names every symbol of a variable `<name>@mem`, whatever its clobber.
    bool testZ3SymbolsWithOneNameStayApart(TestReport& report)
    {
        using namespace ctrace::stack::analysis::smt;
        ConstraintIR ir;
        ir.symbols.push_back(SymbolInfo{.id = 1, .debugName = "x@mem", .sourceToken = 0});
        ir.symbols.push_back(SymbolInfo{.id = 2, .debugName = "x@mem", .sourceToken = 0});
        const auto add = [&](ExprNode node)
        {
            ir.nodes.push_back(node);
            return static_cast<ExprId>(ir.nodes.size() - 1);
        };
        const ExprId before = add({.kind = ExprKind::Symbol, .symbol = 1, .bitWidth = 32});
        const ExprId after = add({.kind = ExprKind::Symbol, .symbol = 2, .bitWidth = 32});
        const ExprId zero = add({.kind = ExprKind::Constant, .constant = 0, .bitWidth = 32});
        const ExprId five = add({.kind = ExprKind::Constant, .constant = 5, .bitWidth = 32});
        ir.assertions.push_back(
            add({.kind = ExprKind::Eq, .bitWidth = 1, .lhs = before, .rhs = zero}));
        ir.assertions.push_back(
            add({.kind = ExprKind::Eq, .bitWidth = 1, .lhs = after, .rhs = five}));

        SmtQuery query;
        query.ir = std::move(ir);
        query.timeoutMs = 1000;
        const SmtDecision decision =
            SolverOrchestrator(SolverOrchestratorConfig{.primaryBackend = "z3"}).solve(query);
        report.expect(decision.status == SmtStatus::Sat,
                      "Z3 backend: two symbols with one name are two variables (x == 0, x' == 5)");
        return report.failures == 0;
    }
#endif

    /// Duplicate-if: GEP indices are compared as signed values, whatever their width. A C
    /// frontend uses one width per element type; IR from elsewhere may not.
    bool testDuplicateIfIndexWidths(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/duplicate_if_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false,
                          "duplicate-if index widths setup: failed to load module: " + loadError);
            return false;
        }

        const auto analyzeAll = [&loaded]
        {
            return analyzeDuplicateIfConditions(*loaded.module,
                                                [](const llvm::Function&) { return true; });
        };
        report.expect(analyzeAll().size() == 1,
                      "duplicate-if: p[-1] tested twice is a duplicate condition");

        std::vector<llvm::GetElementPtrInst*> geps;
        for (llvm::Instruction& inst :
             llvm::instructions(*loaded.module->getFunction("same_element_twice")))
        {
            if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst))
                geps.push_back(gep);
        }
        report.expect(geps.size() == 2, "duplicate-if: each condition indexes p once");
        if (geps.size() != 2)
            return false;

        // i64 -1 becomes i32 -1: the same element, but not the same APInt word.
        geps[1]->setOperand(1, llvm::ConstantInt::getSigned(
                                   llvm::Type::getInt32Ty(loaded.module->getContext()), -1));
        report.expect(analyzeAll().size() == 1,
                      "duplicate-if: the same index as i64 and as i32 is the same element");
        return report.failures == 0;
    }

    /// ProgramPointRanges: an unsigned comparison bounds the unsigned reading of a value. The
    /// signed reading keeps that bound only where the two readings agree.
    bool testProgramPointRangesUnsignedReadings(const std::filesystem::path& repoRoot,
                                                TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const ctrace::stack::AnalysisConfig config;
        LoadedModule loaded;
        std::string loadError;
        if (!loadModuleFromSource(repoRoot / "test/unit/int_range_unsigned_input.c", config, loaded,
                                  loadError))
        {
            report.expect(false, "unsigned readings setup: failed to load module: " + loadError);
            return false;
        }

        // The range of the index at the function's array access. The index is a cast of a
        // load, and the loaded slot is the key.
        const auto rangeAtAccess = [&loaded](const char* name,
                                             IntReading reading) -> std::optional<IntRange>
        {
            llvm::Function* fn = loaded.module->getFunction(name);
            if (!fn)
                return std::nullopt;
            for (llvm::Instruction& inst : llvm::instructions(*fn))
            {
                const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst);
                const auto* cast =
                    gep ? llvm::dyn_cast<llvm::CastInst>(gep->getOperand(gep->getNumOperands() - 1))
                        : nullptr;
                const auto* load =
                    cast ? llvm::dyn_cast<llvm::LoadInst>(cast->getOperand(0)) : nullptr;
                if (!load)
                    continue;
                const FunctionFacts facts(*fn);
                const ProgramPointRanges ranges(*fn, facts);
                return ranges.at(load->getPointerOperand(), *gep, reading);
            }
            return std::nullopt;
        };
        const auto is = [](const std::optional<IntRange>& r, std::optional<long long> lower,
                           std::optional<long long> upper)
        {
            return r && r->hasLower == lower.has_value() && r->hasUpper == upper.has_value() &&
                   (!lower || r->lower == *lower) && (!upper || r->upper == *upper);
        };
        const auto unbounded = [](const std::optional<IntRange>& r)
        { return !r || (!r->hasLower && !r->hasUpper); };

        report.expect(is(rangeAtAccess("ugt_guard", IntReading::Signed), -9, 15),
                      "ProgramPointRanges: under (unsigned)i > 10u, i > -10 && i < 16 give "
                      "i in [-9, 15]");
        report.expect(is(rangeAtAccess("ult_guard", IntReading::Signed), 0, 15),
                      "ProgramPointRanges: (unsigned)i < 16u puts i in [0, 15]");
        report.expect(unbounded(rangeAtAccess("wide_ule_guard", IntReading::Signed)),
                      "ProgramPointRanges: (unsigned)i <= 0x80000000u gives no signed bound");
        report.expect(is(rangeAtAccess("unsigned_index", IntReading::Unsigned), 21, std::nullopt),
                      "ProgramPointRanges: u > 20u puts the unsigned reading of u at 21 or more");
        report.expect(unbounded(rangeAtAccess("unsigned_index", IntReading::Signed)),
                      "ProgramPointRanges: u > 20u gives no bound on the signed reading of u");
        report.expect(is(rangeAtAccess("signed_guard_unsigned_index", IntReading::Unsigned), 21,
                         std::nullopt),
                      "ProgramPointRanges: (int)u > 20 also bounds the unsigned reading of u");
        report.expect(is(rangeAtAccess("unsigned_read_of_signed_guard", IntReading::Unsigned), 0,
                         4294967295LL),
                      "ProgramPointRanges: under i <= 20, (unsigned)i may be any 32-bit value");
        return report.failures == 0;
    }

    /// Tags of rule @p rule in the SARIF log @p sarif, or std::nullopt when the rule is absent.
    std::optional<std::vector<std::string>> sarifRuleTags(const std::string& sarif,
                                                          llvm::StringRef rule)
    {
        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(sarif);
        if (!parsed)
        {
            llvm::consumeError(parsed.takeError());
            return std::nullopt;
        }
        const llvm::json::Object* root = parsed->getAsObject();
        const llvm::json::Array* runs = root ? root->getArray("runs") : nullptr;
        const llvm::json::Object* run = runs && !runs->empty() ? (*runs)[0].getAsObject() : nullptr;
        const llvm::json::Object* tool = run ? run->getObject("tool") : nullptr;
        const llvm::json::Object* driver = tool ? tool->getObject("driver") : nullptr;
        const llvm::json::Array* rules = driver ? driver->getArray("rules") : nullptr;
        if (!rules)
            return std::nullopt;
        for (const llvm::json::Value& entry : *rules)
        {
            const llvm::json::Object* object = entry.getAsObject();
            if (!object || object->getString("id") != rule)
                continue;
            std::vector<std::string> tags;
            if (const llvm::json::Object* properties = object->getObject("properties"))
            {
                if (const llvm::json::Array* list = properties->getArray("tags"))
                {
                    for (const llvm::json::Value& tag : *list)
                    {
                        if (const std::optional<llvm::StringRef> text = tag.getAsString())
                            tags.push_back(text->str());
                    }
                }
            }
            return tags;
        }
        return std::nullopt;
    }

    /// SARIF: a rule lists every CWE of its diagnostics in the run, whatever their order.
    bool testSarifRuleCweTags(TestReport& report)
    {
        const auto diagnostic = [](const char* rule, const char* cwe)
        {
            ctrace::stack::Diagnostic d;
            d.ruleId = rule;
            d.cweId = cwe;
            d.filePath = "demo.c";
            d.line = 1;
            d.column = 1;
            d.message = rule;
            return d;
        };

        ctrace::stack::AnalysisResult readFirst;
        readFirst.diagnostics = {diagnostic("StackBufferOverflow", "CWE-125"),
                                 diagnostic("StackBufferOverflow", "CWE-121"),
                                 diagnostic("StackBufferOverflow", "CWE-125"),
                                 diagnostic("CommandInjection.NonLiteralCommand", "CWE-78")};
        ctrace::stack::AnalysisResult writeFirst;
        writeFirst.diagnostics = {diagnostic("StackBufferOverflow", "CWE-121"),
                                  diagnostic("StackBufferOverflow", "CWE-125"),
                                  diagnostic("CommandInjection.NonLiteralCommand", "CWE-78")};

        // Only the CWE tags: a rule may carry others (the security tag).
        const auto cweTags = [](const ctrace::stack::AnalysisResult& result, llvm::StringRef rule)
        {
            std::optional<std::vector<std::string>> tags =
                sarifRuleTags(ctrace::stack::toSarif(result, "demo.c"), rule);
            if (tags)
                std::erase_if(*tags, [](const std::string& tag)
                              { return !llvm::StringRef(tag).starts_with("external/cwe/"); });
            return tags;
        };

        const std::vector<std::string> both{"external/cwe/cwe-121", "external/cwe/cwe-125"};
        report.expect(cweTags(readFirst, "StackBufferOverflow") == both,
                      "SARIF: a rule lists every CWE of its diagnostics, sorted, once each");
        report.expect(cweTags(writeFirst, "StackBufferOverflow") == both,
                      "SARIF: the CWE tags of a rule do not depend on diagnostic order");
        report.expect(cweTags(readFirst, "CommandInjection.NonLiteralCommand") ==
                          std::vector<std::string>{"external/cwe/cwe-078"},
                      "SARIF: CWE tags are zero-padded to three digits");
        return report.failures == 0;
    }

    /// Properties of rule @p rule in the SARIF log @p sarif (empty when it has none), or
    /// std::nullopt when the rule is absent.
    std::optional<llvm::json::Object> sarifRuleProperties(const std::string& sarif,
                                                          llvm::StringRef rule)
    {
        llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(sarif);
        if (!parsed)
        {
            llvm::consumeError(parsed.takeError());
            return std::nullopt;
        }
        const llvm::json::Object* root = parsed->getAsObject();
        const llvm::json::Array* runs = root ? root->getArray("runs") : nullptr;
        const llvm::json::Object* run = runs && !runs->empty() ? (*runs)[0].getAsObject() : nullptr;
        const llvm::json::Object* tool = run ? run->getObject("tool") : nullptr;
        const llvm::json::Object* driver = tool ? tool->getObject("driver") : nullptr;
        const llvm::json::Array* rules = driver ? driver->getArray("rules") : nullptr;
        if (!rules)
            return std::nullopt;
        for (const llvm::json::Value& entry : *rules)
        {
            const llvm::json::Object* object = entry.getAsObject();
            if (!object || object->getString("id") != rule)
                continue;
            const llvm::json::Object* properties = object->getObject("properties");
            return properties ? *properties : llvm::json::Object{};
        }
        return std::nullopt;
    }

    /// SARIF: every CWE the diagnostics report has a security severity, so the rules that report
    /// it are security rules; dead code (CWE-561), a quality issue for CodeQL, is the exception.
    /// The CWEs are read from the emitters' sources, so a new one cannot be left unscored
    /// unnoticed.
    bool testEveryReportedCweIsScored(const std::filesystem::path& repoRoot, TestReport& report)
    {
        std::set<std::string> cwes;
        for (const char* source :
             {"src/analyzer/DiagnosticEmitter.cpp", "src/analysis/FrontendDiagnostics.cpp"})
        {
            llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
                llvm::MemoryBuffer::getFile((repoRoot / source).string());
            if (!buffer)
                continue;
            const llvm::StringRef text = (*buffer)->getBuffer();
            for (std::size_t at = text.find("\"CWE-"); at != llvm::StringRef::npos;
                 at = text.find("\"CWE-", at + 1))
            {
                cwes.insert(text.substr(at + 1).take_until([](char c) { return c == '"'; }).str());
            }
        }
        report.expect(cwes.count("CWE-121") && cwes.count("CWE-134"),
                      "SARIF: the CWEs of both emitters are read from their sources");

        const auto severity = [](const std::string& rule, const std::string& cwe)
        {
            ctrace::stack::Diagnostic d;
            d.ruleId = rule;
            d.cweId = cwe;
            d.filePath = "demo.c";
            d.line = 1;
            d.column = 1;
            d.message = rule;
            ctrace::stack::AnalysisResult result;
            result.diagnostics = {d};
            const std::optional<llvm::json::Object> properties =
                sarifRuleProperties(ctrace::stack::toSarif(result, "demo.c"), rule);
            const std::optional<llvm::StringRef> score =
                properties ? properties->getString("security-severity") : std::nullopt;
            return score ? score->str() : std::string();
        };

        std::string wrong;
        for (const std::string& cwe : cwes)
        {
            if (severity("Rule", cwe).empty() != (cwe == "CWE-561"))
                wrong += " " + cwe;
        }
        report.expect(wrong.empty(),
                      "SARIF: every reported CWE but dead code (CWE-561) has a security severity" +
                          (wrong.empty() ? std::string() : " (wrong:" + wrong + ")"));
        report.expect(
            severity("Recursion.Unconditional", "CWE-674") == "7.5",
            "SARIF: an unconditional recursion (CWE-674) scores 7.5, as an infinite loop");
        return report.failures == 0;
    }

    /// SARIF: GitHub code scanning ranks the alerts of a rule by its security-severity when the
    /// rule has the security tag. A rule gets both from its scored CWEs, and a hint gets neither.
    bool testSarifSecuritySeverity(TestReport& report)
    {
        const auto diagnostic = [](const char* rule, const char* cwe)
        {
            ctrace::stack::Diagnostic d;
            d.ruleId = rule;
            d.cweId = cwe;
            d.filePath = "demo.c";
            d.line = 1;
            d.column = 1;
            d.message = rule;
            return d;
        };

        ctrace::stack::AnalysisResult result;
        result.diagnostics = {
            diagnostic("StackBufferOverflow", "CWE-121"),
            diagnostic("ConstParameterNotModified.Pointer", ""),
            // The highest score (CWE-457, 7.8) is neither the first CWE nor
            // the last: CWE-200 scores 6.5 and CWE-685 5.0.
            diagnostic("MixedCwes", "CWE-685"), diagnostic("MixedCwes", "CWE-457"),
            diagnostic("MixedCwes", "CWE-200"), diagnostic("DuplicateIfCondition", "CWE-561")};
        const std::string sarif = ctrace::stack::toSarif(result, "demo.c");

        const auto hasSecurityTag = [](const llvm::json::Object& properties)
        {
            const llvm::json::Array* tags = properties.getArray("tags");
            return tags &&
                   llvm::any_of(*tags, [](const llvm::json::Value& tag)
                                { return tag.getAsString() == llvm::StringRef("security"); });
        };
        const auto isSecurityRule = [&](llvm::StringRef rule, llvm::StringRef score)
        {
            const std::optional<llvm::json::Object> properties = sarifRuleProperties(sarif, rule);
            return properties && hasSecurityTag(*properties) &&
                   properties->getString("security-severity") == score;
        };
        const auto isPlainRule = [&](llvm::StringRef rule)
        {
            const std::optional<llvm::json::Object> properties = sarifRuleProperties(sarif, rule);
            return properties && !hasSecurityTag(*properties) &&
                   !properties->get("security-severity");
        };

        report.expect(isSecurityRule("StackBufferOverflow", "9.3"),
                      "SARIF: a rule with a scored CWE has the security tag and its score");
        report.expect(isPlainRule("ConstParameterNotModified.Pointer"),
                      "SARIF: a hint rule has neither the security tag nor a score");
        report.expect(isSecurityRule("MixedCwes", "7.8"),
                      "SARIF: a rule takes the highest score of its CWEs");
        report.expect(isPlainRule("DuplicateIfCondition"),
                      "SARIF: a CWE without a score (dead code) does not make a security rule");
        return report.failures == 0;
    }
} // namespace

namespace
{
    /// An analysis that exhausts its budget reports an Info note under a rule of its own, with
    /// no CWE. Under the rule of the findings, code scanning would show the note as a security
    /// alert with the findings' severity.
    bool testAnalysisIncompleteNotesHaveTheirOwnRules(TestReport& report)
    {
        using namespace ctrace::stack::analysis;

        UninitializedLocalReadIssue read;
        read.funcName = "reads";
        read.varName = "v";
        read.line = 3;
        read.column = 5;
        UninitializedLocalReadIssue readBudget = read;
        readBudget.kind = UninitializedLocalIssueKind::AnalysisIncomplete;
        readBudget.calleeName = "8/4"; // "<iterations>/<blocks>" for this kind

        ResourceLifetimeIssue leak;
        leak.funcName = "leaks";
        leak.resourceKind = "FILE";
        leak.handleName = "fp";
        ResourceLifetimeIssue leakBudget = leak;
        leakBudget.kind = ResourceLifetimeIssueKind::AnalysisIncomplete;

        ctrace::stack::AnalysisResult result;
        ctrace::stack::analyzer::appendUninitializedLocalReadDiagnostics(result,
                                                                         {read, readBudget});
        ctrace::stack::analyzer::appendResourceLifetimeDiagnostics(result, {leak, leakBudget});

        std::set<std::string> noteRules;
        for (const ctrace::stack::Diagnostic& diag : result.diagnostics)
        {
            if (diag.message.find("did not converge") != std::string::npos)
                noteRules.insert(diag.ruleId);
        }
        report.expect(noteRules ==
                          std::set<std::string>{"UninitializedLocalRead.AnalysisIncomplete",
                                                "ResourceLifetime.AnalysisIncomplete"},
                      "AnalysisIncomplete: each note has its own rule");

        // Whether the SARIF rule has the security tag, or std::nullopt when the rule is absent.
        const std::string sarif = ctrace::stack::toSarif(result, "demo.c");
        const auto securityTag = [&sarif](llvm::StringRef rule) -> std::optional<bool>
        {
            const std::optional<llvm::json::Object> properties = sarifRuleProperties(sarif, rule);
            if (!properties)
                return std::nullopt;
            const llvm::json::Array* tags = properties->getArray("tags");
            return tags &&
                   llvm::any_of(*tags, [](const llvm::json::Value& tag)
                                { return tag.getAsString() == llvm::StringRef("security"); });
        };
        report.expect(securityTag("UninitializedLocalRead") == true &&
                          securityTag("ResourceLifetime.MissingRelease") == true,
                      "AnalysisIncomplete: the findings' rules stay security rules");
        report.expect(securityTag("UninitializedLocalRead.AnalysisIncomplete") == false &&
                          securityTag("ResourceLifetime.AnalysisIncomplete") == false,
                      "AnalysisIncomplete: the notes' rules are not security rules");
        return report.failures == 0;
    }

    std::optional<long long> returnedConstant(const llvm::Module& module, llvm::StringRef function)
    {
        const llvm::Function* defined = module.getFunction(function);
        if (defined == nullptr)
            return std::nullopt;
        for (const llvm::BasicBlock& block : *defined)
        {
            const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
            if (ret == nullptr)
                continue;
            if (const auto* value =
                    llvm::dyn_cast_or_null<llvm::ConstantInt>(ret->getReturnValue()))
                return value->getSExtValue();
        }
        return std::nullopt;
    }

    /// The compile IR cache records the coretrace-compiler revision of the build. A build that
    /// does not know it (coretrace-compiler.cmake) must neither read the cache, whose entries
    /// could come from any compiler, nor write it, with entries no build could check.
    ///
    /// An entry is written by a build of revision "known", then changed to return 7 where the
    /// source returns 42. Recorded as revision "other", it is reused by a build of "other": it
    /// is otherwise valid. Recorded with no revision, a build without one must still compile
    /// the source, and leave the cache as it was. Over an empty cache, it must write nothing.
    bool testCompileIRCacheWithoutCompilerRevision(TestReport& report)
    {
        char rootTemplate[] = "/tmp/ctrace-compile-ir-XXXXXX";
        if (mkdtemp(rootTemplate) == nullptr)
        {
            report.expect(false, "Compile IR cache without revision: no temporary directory");
            return false;
        }
        const std::filesystem::path root(rootTemplate);
        const std::filesystem::path source = root / "value.c";
        std::ofstream(source) << "int value(void) { return 42; }\n";

        ctrace::stack::AnalysisConfig config;
        config.compileIRFormat = ctrace::stack::CompileIRFormat::LL;
        const auto load = [&](const std::filesystem::path& cache,
                              llvm::StringRef revision) -> std::optional<long long>
        {
            config.compileIRCacheDir = cache.string();
            llvm::LLVMContext context;
            llvm::SMDiagnostic err;
            const ctrace::stack::analysis::ModuleLoadResult loaded =
                ctrace::stack::analysis::detail::loadModuleForAnalysis(source.string(), config,
                                                                       context, err, revision);
            if (!loaded.module)
                return std::nullopt;
            return returnedConstant(*loaded.module, "value");
        };
        // Every file of the cache, with its contents.
        const auto contents = [](const std::filesystem::path& cache)
        {
            std::map<std::string, std::string> files;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(cache, ec))
            {
                std::ostringstream text;
                text << std::ifstream(entry.path(), std::ios::binary).rdbuf();
                files[entry.path().filename().string()] = text.str();
            }
            return files;
        };
        const auto record = [](const std::filesystem::path& meta, llvm::StringRef revision)
        {
            std::ostringstream text;
            text << std::ifstream(meta).rdbuf();
            llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text.str());
            if (!parsed)
            {
                llvm::consumeError(parsed.takeError());
                return false;
            }
            llvm::json::Object* object = parsed->getAsObject();
            if (object == nullptr)
                return false;
            (*object)["coretraceCompiler"] = revision.str();
            std::string written;
            llvm::raw_string_ostream out(written);
            out << *parsed;
            out.flush();
            std::ofstream(meta, std::ios::trunc) << written;
            return true;
        };

        const std::filesystem::path cache = root / "cache";
        report.expect(load(cache, "known") == 42,
                      "Compile IR cache: a build of a known revision compiles the source");
        std::filesystem::path meta;
        std::string recordedRevision = "<none>";
        for (const auto& [name, text] : contents(cache))
        {
            if (!llvm::StringRef(name).ends_with(".json"))
                continue;
            meta = cache / name;
            llvm::Expected<llvm::json::Value> parsed = llvm::json::parse(text);
            if (!parsed)
            {
                llvm::consumeError(parsed.takeError());
                continue;
            }
            if (const llvm::json::Object* object = parsed->getAsObject())
                if (const auto revision = object->getString("coretraceCompiler"))
                    recordedRevision = revision->str();
        }
        report.expect(recordedRevision == "known",
                      "Compile IR cache: the entry records the build's revision (" +
                          recordedRevision + ")");
        report.expect(!meta.empty(),
                      "Compile IR cache: a build of a known revision writes an entry");
        if (meta.empty())
        {
            std::filesystem::remove_all(root);
            return false;
        }

        const std::filesystem::path ir = std::filesystem::path(meta).replace_extension(".ll");
        std::string irText = contents(cache)[ir.filename().string()];
        const std::size_t at = irText.find("ret i32 42");
        if (at != std::string::npos)
            irText.replace(at, std::string("ret i32 42").size(), "ret i32 7");
        std::ofstream(ir, std::ios::trunc) << irText;
        report.expect(record(meta, "other") && load(cache, "other") == 7,
                      "Compile IR cache: the changed entry is reused by a build of its revision");

        report.expect(record(meta, ""), "Compile IR cache: the entry now records no revision");
        const std::map<std::string, std::string> before = contents(cache);
        report.expect(load(cache, "") == 42,
                      "Compile IR cache: a build without revision does not read the cache");
        report.expect(contents(cache) == before,
                      "Compile IR cache: a build without revision leaves the cache as it was");

        const std::filesystem::path empty = root / "empty-cache";
        report.expect(load(empty, "") == 42 && contents(empty).empty(),
                      "Compile IR cache: a build without revision writes no entry");

        std::filesystem::remove_all(root);
        return report.failures == 0;
    }

    /// A symbol is recognized as mangled and shown demangled when a demangler reads it, whatever
    /// the C++ runtime: one that predates a mangling, such as a C++23 explicit object member's
    /// (macOS 14, GCC 11), leaves it to LLVM's (#240). A name that neither reads stays as it is.
    bool testSymbolDemangling(TestReport& report)
    {
        using ctrace::stack::analysis::formatFunctionNameForMessage;
        const std::string explicitObject = "_ZNH6Reader4peekERKS_Pi";
        const std::string shown = "Reader::peek(this Reader const&, int*)";
        report.expect(ctrace_tools::isMangled(explicitObject),
                      "Demangling: an explicit object member's symbol is a mangled name");
        report.expect(ctrace_tools::demangle(explicitObject.c_str()) == shown,
                      "Demangling: an explicit object member's symbol is shown demangled");
        report.expect(formatFunctionNameForMessage(explicitObject) == shown,
                      "Demangling: a message names an explicit object member demangled");
        report.expect(ctrace_tools::isMangled("_ZN6Reader4peekEPi") &&
                          ctrace_tools::demangle("_ZN6Reader4peekEPi") == "Reader::peek(int*)",
                      "Demangling: a member's symbol is recognized and shown demangled");
        report.expect(!ctrace_tools::isMangled("read_value") &&
                          formatFunctionNameForMessage("read_value") == "read_value",
                      "Demangling: a C function's name is no mangled name");
        report.expect(!ctrace_tools::isMangled("_Z_not_a_symbol") &&
                          ctrace_tools::demangle("_Z_not_a_symbol") == "_Z_not_a_symbol",
                      "Demangling: a name that no demangler reads stays as it is");
        return report.failures == 0;
    }

    /// resolveParameterDebugBinding: each parameter gets its own type, and its position among the
    /// parameters the source declares. The others are artificial: `this`, the vtt that the
    /// base-object constructor (C2) of a class with a virtual base takes, is_most_derived under the
    /// Microsoft ABI, a block's descriptor.
    bool testParameterDebugBinding(const std::filesystem::path& repoRoot, TestReport& report)
    {
        using namespace ctrace::stack::analysis;
        const auto describe = [](const llvm::Function& F)
        {
            std::vector<std::string> parameters;
            for (const llvm::Argument& Arg : F.args())
            {
                const ParameterDebugBinding binding = resolveParameterDebugBinding(F, Arg);
                std::string text = binding.name + ": ";
                if (binding.isArtificial)
                {
                    text += "artificial";
                }
                else if (!binding.type)
                {
                    text += "no type";
                }
                else if (const auto* pointer = llvm::dyn_cast<llvm::DIDerivedType>(binding.type))
                {
                    const llvm::DIType* pointee = pointer->getBaseType();
                    text += (pointee ? pointee->getName().str() : "?") + " *";
                }
                else
                {
                    text += binding.type->getName().str();
                }
                text += ", source " +
                        (binding.sourceIndex ? std::to_string(*binding.sourceIndex) : "none");
                parameters.push_back(text);
            }
            return parameters;
        };
        // The arguments' bindings, in order. An expected entry written "?name: binding" is the
        // parameter of an argument that only some ABIs pass: absent, or at that place with that
        // binding, its IR name the parameter's or derived from it (tag.coerce).
        const auto check = [&](const LoadedModule& loaded, const char* symbol,
                               const std::vector<std::string>& expected)
        {
            const llvm::Function* F = loaded.module->getFunction(symbol);
            const std::vector<std::string> actual = F ? describe(*F) : std::vector<std::string>{};
            std::string text;
            for (const std::string& parameter : actual)
                text += " [" + parameter + "]";
            const auto passes = [](const std::string& parameter, const std::string& optional)
            {
                const std::size_t colon = optional.find(':');
                const std::string name = optional.substr(1, colon - 1);
                return parameter == optional.substr(1) ||
                       (parameter.starts_with(name + ".") &&
                        parameter.substr(parameter.find(':')) == optional.substr(colon));
            };
            std::size_t next = 0;
            bool matches = true;
            for (const std::string& entry : expected)
            {
                if (entry.starts_with("?"))
                {
                    if (next < actual.size() && passes(actual[next], entry))
                        ++next;
                    continue;
                }
                matches = matches && next < actual.size() && actual[next] == entry;
                ++next;
            }
            report.expect(matches && next == actual.size(),
                          std::string("ParameterDebugBinding: ") + symbol + text);
        };
        const auto load =
            [&](const char* input, std::vector<std::string> compileArgs, LoadedModule& out)
        {
            ctrace::stack::AnalysisConfig config;
            config.extraCompileArgs = std::move(compileArgs);
            std::string loadError;
            if (loadModuleFromSource(repoRoot / "test/unit" / input, config, out, loadError))
                return true;
            report.expect(false,
                          "ParameterDebugBinding setup: failed to load module: " + loadError);
            return false;
        };

        LoadedModule itanium;
        LoadedModule microsoft;
        LoadedModule blocks;
        LoadedModule unestablished;
        LoadedModule optimized;
        if (!load("parameter_binding_input.cpp", {"-std=c++23"}, itanium) ||
            !load("parameter_binding_input.cpp", {"-std=c++23", "--target=x86_64-pc-windows-msvc"},
                  microsoft) ||
            !load("parameter_binding_block_input.c", {"-fblocks"}, blocks) ||
            !load("parameter_binding_unknown_input.ll", {}, unestablished) ||
            !load("parameter_binding_o2_input.ll", {}, optimized))
            return false;
        check(itanium, "_Z13free_functionPiPd", {"a: int *, source 0", "b: double *, source 1"});
        check(itanium, "_ZN6Reader3sumEPiPd",
              {"this: artificial, source none", "p: int *, source 0", "q: double *, source 1"});
        check(itanium, "_ZN6ReaderC1EPiPd",
              {"this: artificial, source none", "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZN6ReaderC2EPiPd",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZNH6Picker4pickERKS_Pi",
              {"self: artificial, source 0", "p: int *, source 1"});
        check(microsoft, "??0Reader@@QEAA@PEAHPEAN@Z",
              {"this: artificial, source none", "x: int *, source 0", "y: double *, source 1",
               "is_most_derived: artificial, source none"});
        check(blocks, "__use_block_block_invoke",
              {".block_descriptor: artificial, source none", "x: int *, source 0",
               "y: double *, source 1"});
        // A variadic function's debug type ends with a null entry for `...`, which no parameter
        // takes; an empty parameter keeps its debug declaration, and takes an IR argument only on
        // some ABIs (an i8 under AAPCS64 on Linux, none on x86-64 or Apple arm64).
        check(itanium, "_ZN7VarargsC2EPiPdz",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(microsoft, "??0Varargs@@QEAA@PEAHPEANZZ",
              {"this: artificial, source none", "is_most_derived: artificial, source none",
               "x: int *, source 0", "y: double *, source 1"});
        check(itanium, "_ZN6TaggedC2EPi3Tag",
              {"this: artificial, source none", "vtt: artificial, source none",
               "x: int *, source 0", "?tag: Tag, source 1"});
        check(itanium, "_Z8make_bigPiz",
              {"agg.result: artificial, source none", "p: int *, source 0"});

        // A declaration's type is kept; without one, the debug type serves only where its
        // entries are established.
        check(unestablished, "ambiguous_typed",
              {"hidden: artificial, source none", "x: double *, source 0"});
        check(unestablished, "ambiguous_untyped",
              {"hidden: artificial, source none", "x: no type, source 0"});
        check(unestablished, "established_untyped", {"x: double *, source 0"});

        // The empty parameter comes first: x is the second parameter the source declares.
        check(itanium, "_ZN8TagFirstC2E3TagPi",
              {"this: artificial, source none", "vtt: artificial, source none",
               "?tag: Tag, source 0", "x: int *, source 1"});

        // Optimized code spills nothing: each argument is bound through its own debug records.
        check(optimized, "_ZN8VariadicC2EPiPdz",
              {"this: artificial, source none", "vtt: artificial, source none",
               "v: int *, source none", "w: double *, source none"});
        check(optimized, "split_variadic",
              {"p: int *, source none", "s: Pair, source none", "s: Pair, source none"});

        // Under the Microsoft ABI the hidden return argument comes after `this`: it has no type,
        // so it carries no const contract.
        if (const llvm::Function* make =
                microsoft.module->getFunction("?make@Maker@@QEBA?AUBig@@PEAH@Z"))
        {
            const ParameterDebugBinding hiddenReturn =
                resolveParameterDebugBinding(*make, *make->getArg(1));
            report.expect(hiddenReturn.isArtificial && !hiddenReturn.type,
                          "ParameterDebugBinding: the Microsoft ABI's hidden return argument has "
                          "no type");
        }
        else
        {
            report.expect(false,
                          "ParameterDebugBinding: ?make@Maker@@QEBA?AUBig@@PEAH@Z is missing");
        }

        // What the rule displays for x does not depend on the constructor variant.
        const llvm::Function* complete = itanium.module->getFunction("_ZN6ReaderC1EPiPd");
        const llvm::Function* base = itanium.module->getFunction("_ZN6ReaderC2EPiPd");
        if (complete && base)
        {
            const ParameterDebugBinding inComplete =
                resolveParameterDebugBinding(*complete, *complete->getArg(1));
            const ParameterDebugBinding inBase =
                resolveParameterDebugBinding(*base, *base->getArg(2));
            report.expect(inComplete.name == inBase.name && inComplete.type == inBase.type &&
                              inComplete.line == inBase.line &&
                              inComplete.column == inBase.column &&
                              inComplete.confidence == inBase.confidence,
                          "ParameterDebugBinding: x binds alike in C1 and C2");
        }
        return report.failures == 0;
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
    (void)testIntRangeFacts(repoRoot, report);
    (void)testEveryReportedCweIsScored(repoRoot, report);
    (void)testAnalysisReportContract(repoRoot, report);
    (void)testSarifRuleCweTags(report);
    (void)testSarifSecuritySeverity(report);
    (void)testAnalysisIncompleteNotesHaveTheirOwnRules(report);
    (void)testConstSuggestionCodeComparison(report);
    (void)testConstSuggestionNormalizations(report);
    (void)testConstSuggestionTopLevelQualifierLost(report);
    (void)testConstSuggestionCorrespondenceIndex(report);
    (void)testConstSuggestionNestedCorrespondences(report);
    (void)testConstSuggestionNestedIdentities(report);
    (void)testConstSuggestionImplicitMemberIdentities(report);
    (void)testUnresolvedCallsMarkStackUnknown(repoRoot, report);
    (void)testAssumeExternalFrameReplacesUnknown(repoRoot, report);
    (void)testUninitializedFixpointBudgetIsExplicit(repoRoot, report);
    (void)testLinkerSymbolName(report);
    (void)testParameterDebugBinding(repoRoot, report);
    (void)testUninitializedSummaryKeysAreLinkerSymbols(repoRoot, report);
    (void)testUninitializedIncompleteSummaries(repoRoot, report);
    (void)testUninitializedIncompleteSummariesInOneFile(repoRoot, report);
    (void)testCrossTUDriverMarksUnconvergedCycles(report);
    (void)testProgramPointRanges(repoRoot, report);
    (void)testProgramPointRangesUnsignedReadings(repoRoot, report);
    (void)testSmtEvaluatorEncodesLazily(report);
    (void)testFunctionFactsClobberingAccess(repoRoot, report);
    (void)testSmtEncoderMemoryModel(repoRoot, report);
    (void)testSmtEncoderPathCondition(repoRoot, report);
#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
    (void)testZ3WideNegativeConstant(report);
    (void)testZ3SymbolsWithOneNameStayApart(report);
#endif
    (void)testDuplicateIfIndexWidths(repoRoot, report);
    (void)testResourceModelConditions(report);
    (void)testOwnershipFactCollector(repoRoot, report);
    (void)testOwnershipCollectorExceptions(repoRoot, report);
    (void)testOwnershipSummaryIndex(repoRoot, report);
    (void)testCompileIRCacheWithoutCompilerRevision(report);
    (void)testSymbolDemangling(report);

    if (report.failures == 0)
    {
        std::cout << "All analyzer module unit tests passed.\n";
        return 0;
    }

    std::cerr << report.failures << " analyzer module unit test(s) failed.\n";
    return 1;
}
