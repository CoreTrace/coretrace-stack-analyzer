// SPDX-License-Identifier: Apache-2.0
#include "StackUsageAnalyzer.hpp"
#include "app/AnalyzerApp.hpp"
#include "cli/ArgParser.hpp"
#include "analysis/FunctionFacts.hpp"
#include "analysis/InputPipeline.hpp"
#include "analysis/IntRanges.hpp"
#include "analysis/Reachability.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analysis/UninitializedVarAnalysis.hpp"
#include "analyzer/LocationResolver.hpp"
#include "analyzer/ModulePreparationService.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
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
            const auto it = summaries.functions.find("fill");
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
            const auto it = summaries.functions.find("fill");
            const bool downgraded = it != summaries.functions.end() &&
                                    !it->second.paramEffects.empty() &&
                                    it->second.paramEffects[0].writeRanges.empty() &&
                                    it->second.paramEffects[0].pointerSlotWrites.empty() &&
                                    it->second.paramEffects[0].hasUnknownWrite;
            report.expect(
                downgraded,
                "UninitFixpointBudget: non-converged summary downgrades writes to unknown");
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
    (void)testAnalysisReportContract(repoRoot, report);
    (void)testUnresolvedCallsMarkStackUnknown(repoRoot, report);
    (void)testAssumeExternalFrameReplacesUnknown(repoRoot, report);
    (void)testUninitializedFixpointBudgetIsExplicit(repoRoot, report);

    if (report.failures == 0)
    {
        std::cout << "All analyzer module unit tests passed.\n";
        return 0;
    }

    std::cerr << report.failures << " analyzer module unit test(s) failed.\n";
    return 1;
}
