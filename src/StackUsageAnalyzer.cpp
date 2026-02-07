#include "StackUsageAnalyzer.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <unordered_set>
#include <iostream>
#include <algorithm>
#include <sstream>

#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>

#include "analysis/AllocaUsage.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/ConstParamAnalysis.hpp"
#include "analysis/DuplicateIfCondition.hpp"
#include "analysis/DynamicAlloca.hpp"
#include "analysis/FunctionFilter.hpp"
#include "analysis/InputPipeline.hpp"
#include "analysis/IRValueUtils.hpp"
#include "analysis/InvalidBaseReconstruction.hpp"
#include "analysis/MemIntrinsicOverflow.hpp"
#include "analysis/SizeMinusKWrites.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analysis/StackComputation.hpp"
#include "analysis/StackPointerEscape.hpp"
#include "passes/ModulePasses.hpp"

namespace ctrace::stack
{

    namespace
    {
        struct SourceLocation
        {
            unsigned line = 0;
            unsigned column = 0;
        };

        struct FunctionAuxData
        {
            llvm::DenseMap<const llvm::Function*, SourceLocation> locations;
            llvm::DenseMap<const llvm::Function*, std::string> callPaths;
            llvm::DenseMap<const llvm::Function*, std::vector<std::pair<std::string, StackSize>>>
                localAllocas;
            llvm::DenseMap<const llvm::Function*, std::size_t> indices;
        };

        struct ModuleAnalysisContext
        {
            llvm::Module& mod;
            const AnalysisConfig& config;
            const llvm::DataLayout* dataLayout = nullptr;
            analysis::FunctionFilter filter;
            std::vector<llvm::Function*> functions;
            std::unordered_set<const llvm::Function*> functionSet;
            std::vector<llvm::Function*> allDefinedFunctions;
            std::unordered_set<const llvm::Function*> allDefinedSet;

            bool shouldAnalyze(const llvm::Function& F) const
            {
                return functionSet.find(&F) != functionSet.end();
            }

            bool isDefined(const llvm::Function& F) const
            {
                return allDefinedSet.find(&F) != allDefinedSet.end();
            }
        };

        using LocalStackMap = std::map<const llvm::Function*, analysis::LocalStackInfo>;

        static ModuleAnalysisContext buildContext(llvm::Module& mod, const AnalysisConfig& config)
        {
            ModuleAnalysisContext ctx{mod, config, &mod.getDataLayout(),
                                      analysis::buildFunctionFilter(mod, config)};

            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                ctx.allDefinedFunctions.push_back(&F);
                if (ctx.filter.shouldAnalyze(F))
                    ctx.functions.push_back(&F);
            }
            ctx.allDefinedSet.reserve(ctx.allDefinedFunctions.size());
            for (const llvm::Function* F : ctx.allDefinedFunctions)
            {
                ctx.allDefinedSet.insert(F);
            }
            ctx.functionSet.reserve(ctx.functions.size());
            for (const llvm::Function* F : ctx.functions)
            {
                ctx.functionSet.insert(F);
            }

            return ctx;
        }

        static LocalStackMap computeLocalStacks(const ModuleAnalysisContext& ctx)
        {
            LocalStackMap localStack;
            for (llvm::Function* F : ctx.allDefinedFunctions)
            {
                analysis::LocalStackInfo info =
                    analysis::computeLocalStack(*F, *ctx.dataLayout, ctx.config.mode);
                localStack[F] = info;
            }
            return localStack;
        }

        static analysis::CallGraph buildCallGraphFiltered(const ModuleAnalysisContext& ctx)
        {
            analysis::CallGraph CG;
            for (llvm::Function* F : ctx.allDefinedFunctions)
            {
                auto& vec = CG[F];

                for (llvm::BasicBlock& BB : *F)
                {
                    for (llvm::Instruction& I : BB)
                    {
                        const llvm::Function* Callee = nullptr;
                        if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                        {
                            Callee = CI->getCalledFunction();
                        }
                        else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                        {
                            Callee = II->getCalledFunction();
                        }

                        if (Callee && !Callee->isDeclaration() && ctx.isDefined(*Callee))
                        {
                            vec.push_back(Callee);
                        }
                    }
                }
            }

            return CG;
        }

        static analysis::InternalAnalysisState
        computeRecursionState(const ModuleAnalysisContext& ctx, const analysis::CallGraph& CG,
                              const LocalStackMap& localStack)
        {
            analysis::InternalAnalysisState state =
                analysis::computeGlobalStackUsage(CG, localStack);

            for (llvm::Function* F : ctx.allDefinedFunctions)
            {
                const llvm::Function* Fn = F;
                if (!state.RecursiveFuncs.count(Fn))
                    continue;

                if (analysis::detectInfiniteSelfRecursion(*F))
                {
                    state.InfiniteRecursionFuncs.insert(Fn);
                }
            }

            return state;
        }

        static AnalysisResult buildResults(const ModuleAnalysisContext& ctx,
                                           const LocalStackMap& localStack,
                                           const analysis::InternalAnalysisState& state,
                                           const analysis::CallGraph& CG, FunctionAuxData& aux)
        {
            AnalysisResult result;
            result.config = ctx.config;

            for (llvm::Function* F : ctx.functions)
            {
                const llvm::Function* Fn = F;

                analysis::LocalStackInfo localInfo;
                analysis::StackEstimate totalInfo;

                auto itLocal = localStack.find(Fn);
                if (itLocal != localStack.end())
                    localInfo = itLocal->second;

                auto itTotal = state.TotalStack.find(Fn);
                if (itTotal != state.TotalStack.end())
                    totalInfo = itTotal->second;

                FunctionResult fr;
                fr.name = F->getName().str();
                fr.filePath = analysis::getFunctionSourcePath(*F);
                if (fr.filePath.empty() && !ctx.filter.moduleSourcePath.empty())
                    fr.filePath = ctx.filter.moduleSourcePath;
                fr.localStack = localInfo.bytes;
                fr.localStackUnknown = localInfo.unknown;
                fr.maxStack = totalInfo.bytes;
                fr.maxStackUnknown = totalInfo.unknown;
                fr.hasDynamicAlloca = localInfo.hasDynamicAlloca;
                fr.isRecursive = state.RecursiveFuncs.count(Fn) != 0;
                fr.hasInfiniteSelfRecursion = state.InfiniteRecursionFuncs.count(Fn) != 0;
                fr.exceedsLimit = (!fr.maxStackUnknown && totalInfo.bytes > ctx.config.stackLimit);

                unsigned line = 0;
                unsigned column = 0;
                if (analysis::getFunctionSourceLocation(*F, line, column))
                {
                    aux.locations[Fn] = {line, column};
                }
                if (!fr.isRecursive && totalInfo.bytes > localInfo.bytes)
                {
                    std::string path = analysis::buildMaxStackCallPath(Fn, CG, state);
                    if (!path.empty())
                        aux.callPaths[Fn] = path;
                }
                if (!localInfo.localAllocas.empty())
                {
                    aux.localAllocas[Fn] = localInfo.localAllocas;
                }

                result.functions.push_back(std::move(fr));
                aux.indices[Fn] = result.functions.size() - 1;
            }

            return result;
        }

        static void emitSummaryDiagnostics(AnalysisResult& result, const ModuleAnalysisContext& ctx,
                                           const FunctionAuxData& aux)
        {
            for (const llvm::Function* Fn : ctx.functions)
            {
                auto itIndex = aux.indices.find(Fn);
                if (itIndex == aux.indices.end())
                    continue;
                const std::size_t index = itIndex->second;
                if (index >= result.functions.size())
                    continue;
                const FunctionResult& fr = result.functions[index];

                if (fr.isRecursive)
                {
                    Diagnostic diag;
                    diag.funcName = fr.name;
                    diag.filePath = fr.filePath;
                    diag.severity = DiagnosticSeverity::Warning;
                    diag.errCode = DescriptiveErrorCode::None;
                    diag.message = "  [!] recursive or mutually recursive function detected\n";
                    result.diagnostics.push_back(std::move(diag));
                }

                if (fr.hasInfiniteSelfRecursion)
                {
                    Diagnostic diag;
                    diag.funcName = fr.name;
                    diag.filePath = fr.filePath;
                    diag.severity = DiagnosticSeverity::Warning;
                    diag.errCode = DescriptiveErrorCode::None;
                    diag.message = "  [!!!] unconditional self recursion detected (no base case)\n"
                                   "       this will eventually overflow the stack at runtime\n";
                    result.diagnostics.push_back(std::move(diag));
                }

                if (fr.exceedsLimit)
                {
                    Diagnostic diag;
                    diag.funcName = fr.name;
                    diag.filePath = fr.filePath;
                    diag.severity = DiagnosticSeverity::Warning;
                    diag.errCode = DescriptiveErrorCode::None;
                    auto itLoc = aux.locations.find(Fn);
                    if (itLoc != aux.locations.end())
                    {
                        diag.line = itLoc->second.line;
                        diag.column = itLoc->second.column;
                    }
                    std::string message;
                    bool suppressLocation = false;
                    StackSize maxCallee =
                        (fr.maxStack > fr.localStack) ? (fr.maxStack - fr.localStack) : 0;
                    auto itLocals = aux.localAllocas.find(Fn);
                    std::string aliasLine;
                    if (fr.localStack >= maxCallee && itLocals != aux.localAllocas.end())
                    {
                        std::string localsDetails;
                        std::string singleName;
                        StackSize singleSize = 0;
                        for (const auto& entry : itLocals->second)
                        {
                            if (entry.first == "<unnamed>")
                                continue;
                            if (entry.second >= ctx.config.stackLimit && entry.second > singleSize)
                            {
                                singleName = entry.first;
                                singleSize = entry.second;
                            }
                        }
                        if (!singleName.empty())
                        {
                            aliasLine = "       alias path: " + singleName + "\n";
                        }
                        else if (!itLocals->second.empty())
                        {
                            localsDetails +=
                                "        locals: " + std::to_string(itLocals->second.size()) +
                                " variables (total " + std::to_string(fr.localStack) + " bytes)\n";

                            std::vector<std::pair<std::string, StackSize>> named = itLocals->second;
                            named.erase(std::remove_if(named.begin(), named.end(), [](const auto& v)
                                                       { return v.first == "<unnamed>"; }),
                                        named.end());
                            std::sort(named.begin(), named.end(),
                                      [](const auto& a, const auto& b)
                                      {
                                          if (a.second != b.second)
                                              return a.second > b.second;
                                          return a.first < b.first;
                                      });
                            if (!named.empty())
                            {
                                constexpr std::size_t kMaxLocalsForLocation = 5;
                                if (named.size() > kMaxLocalsForLocation)
                                    suppressLocation = true;
                                std::string listLine = "        locals list: ";
                                for (std::size_t idx = 0; idx < named.size(); ++idx)
                                {
                                    if (idx > 0)
                                        listLine += ", ";
                                    listLine += named[idx].first + "(" +
                                                std::to_string(named[idx].second) + ")";
                                }
                                localsDetails += listLine + "\n";
                            }
                        }
                        if (!localsDetails.empty())
                            message += localsDetails;
                    }
                    auto itPath = aux.callPaths.find(Fn);
                    std::string suffix;
                    if (itPath != aux.callPaths.end())
                    {
                        suffix += "    path: " + itPath->second + "\n";
                    }
                    std::string mainLine = "  [!] potential stack overflow: exceeds limit of " +
                                           std::to_string(ctx.config.stackLimit) + " bytes\n";
                    message = mainLine + aliasLine + suffix + message;
                    if (suppressLocation)
                    {
                        diag.line = 0;
                        diag.column = 0;
                    }
                    diag.message = std::move(message);
                    result.diagnostics.push_back(std::move(diag));
                }
            }
        }

        static void appendStackBufferDiagnostics(
            AnalysisResult& result,
            const std::vector<analysis::StackBufferOverflowIssue>& bufferIssues)
        {
            for (const auto& issue : bufferIssues)
            {
                unsigned line = 0;
                unsigned column = 0;
                unsigned startLine = 0;
                unsigned startColumn = 0;
                unsigned endLine = 0;
                unsigned endColumn = 0;
                bool haveLoc = false;

                if (issue.inst)
                {
                    llvm::DebugLoc DL = issue.inst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        startLine = DL.getLine();

                        startColumn = DL.getCol();
                        column = DL.getCol();

                        // By default, same as start
                        endLine = DL.getLine();
                        endColumn = DL.getCol();
                        haveLoc = true;
                        if (auto* loc = DL.get())
                        {
                            if (auto* scope = llvm::dyn_cast<llvm::DILocation>(loc))
                            {
                                if (scope->getColumn() != 0)
                                {
                                    endColumn = scope->getColumn() + 1;
                                }
                            }
                        }
                    }
                }

                bool isUnreachable = false;
                {
                    using namespace llvm;

                    if (issue.inst)
                    {
                        auto* BB = issue.inst->getParent();

                        // Walk block predecessors to see whether some
                        // have a conditional branch with a constant condition.
                        for (auto* Pred : predecessors(BB))
                        {
                            auto* BI = dyn_cast<BranchInst>(Pred->getTerminator());
                            if (!BI || !BI->isConditional())
                                continue;

                            auto* CI = dyn_cast<ICmpInst>(BI->getCondition());
                            if (!CI)
                                continue;

                            const llvm::Function& Func = *issue.inst->getFunction();

                            auto* C0 = analysis::tryGetConstFromValue(CI->getOperand(0), Func);
                            auto* C1 = analysis::tryGetConstFromValue(CI->getOperand(1), Func);
                            if (!C0 || !C1)
                                continue;

                            // Evaluate the ICmp result for these constants (homegrown implementation).
                            bool condTrue = false;
                            auto pred = CI->getPredicate();
                            const auto& v0 = C0->getValue();
                            const auto& v1 = C1->getValue();

                            switch (pred)
                            {
                            case ICmpInst::ICMP_EQ:
                                condTrue = (v0 == v1);
                                break;
                            case ICmpInst::ICMP_NE:
                                condTrue = (v0 != v1);
                                break;
                            case ICmpInst::ICMP_SLT:
                                condTrue = v0.slt(v1);
                                break;
                            case ICmpInst::ICMP_SLE:
                                condTrue = v0.sle(v1);
                                break;
                            case ICmpInst::ICMP_SGT:
                                condTrue = v0.sgt(v1);
                                break;
                            case ICmpInst::ICMP_SGE:
                                condTrue = v0.sge(v1);
                                break;
                            case ICmpInst::ICMP_ULT:
                                condTrue = v0.ult(v1);
                                break;
                            case ICmpInst::ICMP_ULE:
                                condTrue = v0.ule(v1);
                                break;
                            case ICmpInst::ICMP_UGT:
                                condTrue = v0.ugt(v1);
                                break;
                            case ICmpInst::ICMP_UGE:
                                condTrue = v0.uge(v1);
                                break;
                            default:
                                // Do not handle other exotic predicates here.
                                continue;
                            }

                            // Branch of the form:
                            //   br i1 %cond, label %then, label %else
                            // Successor 0 taken if condTrue == true
                            // Successor 1 taken if condTrue == false
                            if (BB == BI->getSuccessor(0) && condTrue == false)
                            {
                                // The "then" block is never reached.
                                isUnreachable = true;
                            }
                            else if (BB == BI->getSuccessor(1) && condTrue == true)
                            {
                                // The "else" block is never reached.
                                isUnreachable = true;
                            }
                        }
                    }
                }

                std::ostringstream body;
                Diagnostic diag;

                if (issue.isLowerBoundViolation)
                {
                    diag.errCode = DescriptiveErrorCode::NegativeStackIndex;
                    body << "  [!!] potential negative index on variable '" << issue.varName
                         << "' (size " << issue.arraySize << ")\n";
                    if (!issue.aliasPath.empty())
                    {
                        body << "       alias path: " << issue.aliasPath << "\n";
                    }
                    body << "       inferred lower bound for index expression: " << issue.lowerBound
                         << " (index may be < 0)\n";
                }
                else
                {
                    diag.errCode = DescriptiveErrorCode::StackBufferOverflow;
                    body << "  [!!] potential stack buffer overflow on variable '" << issue.varName
                         << "' (size " << issue.arraySize << ")\n";
                    if (!issue.aliasPath.empty())
                    {
                        body << "       alias path: " << issue.aliasPath << "\n";
                    }
                    if (issue.indexIsConstant)
                    {
                        body << "       constant index " << issue.indexOrUpperBound
                             << " is out of bounds (0.."
                             << (issue.arraySize ? issue.arraySize - 1 : 0) << ")\n";
                    }
                    else
                    {
                        body << "       index variable may go up to " << issue.indexOrUpperBound
                             << " (array last valid index: "
                             << (issue.arraySize ? issue.arraySize - 1 : 0) << ")\n";
                    }
                }

                if (issue.isWrite)
                {
                    body << "       (this is a write access)\n";
                }
                else
                {
                    body << "       (this is a read access)\n";
                }
                if (isUnreachable)
                {
                    body << "       [info] this access appears unreachable at runtime "
                            "(condition is always false for this branch)\n";
                }

                diag.funcName = issue.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.startLine = haveLoc ? startLine : 0;
                diag.startColumn = haveLoc ? startColumn : 0;
                diag.endLine = haveLoc ? endLine : 0;
                diag.endColumn = haveLoc ? endColumn : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.message = body.str();
                diag.variableAliasingVec = issue.aliasPathVec;
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendDynamicAllocaDiagnostics(AnalysisResult& result,
                                       const std::vector<analysis::DynamicAllocaIssue>& issues)
        {
            for (const auto& d : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (d.allocaInst)
                {
                    llvm::DebugLoc DL = d.allocaInst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                std::ostringstream body;

                body << "  [!] dynamic stack allocation detected for variable '" << d.varName
                     << "'\n";
                body << "       allocated type: " << d.typeName << "\n";
                body << "       size of this allocation is not compile-time constant "
                        "(VLA / variable alloca) and may lead to unbounded stack usage\n";

                Diagnostic diag;
                diag.funcName = d.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::VLAUsage;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendAllocaUsageDiagnostics(AnalysisResult& result, const AnalysisConfig& config,
                                     StackSize allocaLargeThreshold,
                                     const std::vector<analysis::AllocaUsageIssue>& issues)
        {
            for (const auto& a : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (a.allocaInst)
                {
                    llvm::DebugLoc DL = a.allocaInst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                bool isOversized = false;
                if (a.sizeIsConst && a.sizeBytes >= allocaLargeThreshold)
                    isOversized = true;
                else if (a.hasUpperBound && a.upperBoundBytes >= allocaLargeThreshold)
                    isOversized = true;
                else if (a.sizeIsConst && config.stackLimit != 0 &&
                         a.sizeBytes >= config.stackLimit)
                    isOversized = true;

                std::ostringstream body;
                Diagnostic diag;
                diag.funcName = a.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;

                if (isOversized)
                {
                    diag.severity = DiagnosticSeverity::Error;
                    diag.errCode = DescriptiveErrorCode::AllocaTooLarge;
                    body << "  [!!] large alloca on the stack for variable '" << a.varName << "'\n";
                }
                else if (a.userControlled)
                {
                    diag.severity = DiagnosticSeverity::Warning;
                    diag.errCode = DescriptiveErrorCode::AllocaUserControlled;
                    body << "  [!!] user-controlled alloca size for variable '" << a.varName
                         << "'\n";
                }
                else
                {
                    diag.severity = DiagnosticSeverity::Warning;
                    diag.errCode = DescriptiveErrorCode::AllocaUsageWarning;
                    body << "  [!] dynamic alloca on the stack for variable '" << a.varName
                         << "'\n";
                }

                body
                    << "       allocation performed via alloca/VLA; stack usage grows with runtime "
                       "value\n";

                if (a.sizeIsConst)
                {
                    body << "       requested stack size: " << a.sizeBytes << " bytes\n";
                }
                else if (a.hasUpperBound)
                {
                    body << "       inferred upper bound for size: " << a.upperBoundBytes
                         << " bytes\n";
                }
                else
                {
                    body << "       size is unbounded at compile time\n";
                }

                if (a.isInfiniteRecursive)
                {
                    // Any alloca inside infinite recursion will blow the stack.
                    diag.severity = DiagnosticSeverity::Error;
                    body << "       function is infinitely recursive; this alloca runs at every "
                            "frame and guarantees stack overflow\n";
                }
                else if (a.isRecursive)
                {
                    // Controlled recursion still compounds stack usage across frames.
                    if (diag.severity != DiagnosticSeverity::Error &&
                        (isOversized || a.userControlled))
                    {
                        diag.severity = DiagnosticSeverity::Error;
                    }
                    body << "       function is recursive; this allocation repeats at each "
                            "recursion "
                            "depth and can exhaust the stack\n";
                }

                if (isOversized)
                {
                    body << "       exceeds safety threshold of " << allocaLargeThreshold
                         << " bytes";
                    if (config.stackLimit != 0)
                    {
                        body << " (stack limit: " << config.stackLimit << " bytes)";
                    }
                    body << "\n";
                }
                else if (a.userControlled)
                {
                    body << "       size depends on user-controlled input "
                            "(function argument or non-local value)\n";
                }
                else
                {
                    body << "       size does not appear user-controlled but remains "
                            "runtime-dependent\n";
                }

                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendMemIntrinsicDiagnostics(AnalysisResult& result,
                                      const std::vector<analysis::MemIntrinsicIssue>& issues)
        {
            for (const auto& m : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (m.inst)
                {
                    llvm::DebugLoc DL = m.inst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                std::ostringstream body;

                body << "Function: " << m.funcName;
                if (haveLoc)
                {
                    body << " (line " << line << ", column " << column << ")";
                }
                body << "\n";

                body << "  [!!] potential stack buffer overflow in " << m.intrinsicName
                     << " on variable '" << m.varName << "'\n";
                body << "       destination stack buffer size: " << m.destSizeBytes << " bytes\n";
                body << "       requested " << m.lengthBytes << " bytes to be copied/initialized\n";

                Diagnostic diag;
                diag.funcName = m.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendSizeMinusKDiagnostics(AnalysisResult& result,
                                    const std::vector<analysis::SizeMinusKWriteIssue>& issues)
        {
            for (const auto& s : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (s.inst)
                {
                    llvm::DebugLoc DL = s.inst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                std::ostringstream body;
                if (s.hasPointerDest)
                {
                    body << "  [!] potential unsafe write with length (size - " << s.k << ")";
                }
                else
                {
                    body << "  [!] potential unsafe size-" << s.k << " argument passed";
                }
                if (!s.sinkName.empty())
                    body << " in " << s.sinkName;
                body << "\n";
                if (s.hasPointerDest && !s.ptrNonNull)
                    body << "       destination pointer may be null\n";
                if (!s.sizeAboveK)
                    body << "       size operand may be <= " << s.k << "\n";

                Diagnostic diag;
                diag.funcName = s.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::SizeMinusOneWrite;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendMultipleStoreDiagnostics(AnalysisResult& result,
                                       const std::vector<analysis::MultipleStoreIssue>& issues)
        {
            for (const auto& ms : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (ms.allocaInst)
                {
                    llvm::DebugLoc DL = ms.allocaInst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                std::ostringstream body;
                Diagnostic diag;

                body << "  [!Info] multiple stores to stack buffer '" << ms.varName
                     << "' in this function (" << ms.storeCount << " store instruction(s)";
                diag.errCode = DescriptiveErrorCode::MultipleStoresToStackBuffer;
                if (ms.distinctIndexCount > 0)
                {
                    body << ", " << ms.distinctIndexCount << " distinct index expression(s)";
                }
                body << ")\n";

                if (ms.distinctIndexCount == 1)
                {
                    body << "       all stores use the same index expression "
                            "(possible redundant or unintended overwrite)\n";
                }
                else if (ms.distinctIndexCount > 1)
                {
                    body << "       stores use different index expressions; "
                            "verify indices are correct and non-overlapping\n";
                }

                diag.funcName = ms.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.severity = DiagnosticSeverity::Info;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void appendDuplicateIfConditionDiagnostics(
            AnalysisResult& result, const std::vector<analysis::DuplicateIfConditionIssue>& issues)
        {
            for (const auto& issue : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                unsigned startLine = 0;
                unsigned startColumn = 0;
                unsigned endLine = 0;
                unsigned endColumn = 0;
                bool haveLoc = false;

                if (issue.conditionInst)
                {
                    llvm::DebugLoc DL = issue.conditionInst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        startLine = DL.getLine();
                        column = DL.getCol();
                        startColumn = DL.getCol();
                        endLine = DL.getLine();
                        endColumn = DL.getCol();
                        haveLoc = true;

                        if (auto* loc = DL.get())
                        {
                            if (auto* scope = llvm::dyn_cast<llvm::DILocation>(loc))
                            {
                                if (scope->getColumn() != 0)
                                {
                                    endColumn = scope->getColumn() + 1;
                                }
                            }
                        }
                    }
                }

                std::ostringstream body;
                body << "  [!] unreachable else-if branch: condition is equivalent to a previous "
                        "'if' condition\n";
                body << "       else branch implies previous condition is false\n";

                Diagnostic diag;
                diag.funcName = issue.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.startLine = haveLoc ? startLine : 0;
                diag.startColumn = haveLoc ? startColumn : 0;
                diag.endLine = haveLoc ? endLine : 0;
                diag.endColumn = haveLoc ? endColumn : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::DuplicateIfCondition;
                diag.ruleId = "DuplicateIfCondition";
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void appendInvalidBaseReconstructionDiagnostics(
            AnalysisResult& result,
            const std::vector<analysis::InvalidBaseReconstructionIssue>& issues)
        {
            for (const auto& br : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                unsigned startLine = 0;
                unsigned startColumn = 0;
                unsigned endLine = 0;
                unsigned endColumn = 0;
                bool haveLoc = false;

                if (br.inst)
                {
                    llvm::DebugLoc DL = br.inst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        startLine = DL.getLine();
                        startColumn = DL.getCol();
                        column = DL.getCol();
                        endLine = DL.getLine();
                        endColumn = DL.getCol();
                        haveLoc = true;

                        if (auto* loc = DL.get())
                        {
                            if (auto* scope = llvm::dyn_cast<llvm::DILocation>(loc))
                            {
                                if (scope->getColumn() != 0)
                                {
                                    endColumn = scope->getColumn() + 1;
                                }
                            }
                        }
                    }
                }

                std::ostringstream body;

                body << "  [!!] potential UB: invalid base reconstruction via "
                        "offsetof/container_of\n";
                body << "       variable: '" << br.varName << "'\n";
                body << "       source member: " << br.sourceMember << "\n";
                body << "       offset applied: " << (br.offsetUsed >= 0 ? "+" : "")
                     << br.offsetUsed << " bytes\n";
                body << "       target type: " << br.targetType << "\n";

                if (br.isOutOfBounds)
                {
                    body
                        << "       [ERROR] derived pointer points OUTSIDE the valid object range\n";
                    body << "               (this will cause undefined behavior if dereferenced)\n";
                }
                else
                {
                    body << "       [WARNING] unable to verify that derived pointer points to a "
                            "valid "
                            "object\n";
                    body << "                 (potential undefined behavior if offset is "
                            "incorrect)\n";
                }

                Diagnostic diag;
                diag.funcName = br.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.startLine = haveLoc ? startLine : 0;
                diag.startColumn = haveLoc ? startColumn : 0;
                diag.endLine = haveLoc ? endLine : 0;
                diag.endColumn = haveLoc ? endColumn : 0;
                diag.severity =
                    br.isOutOfBounds ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::InvalidBaseReconstruction;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void appendStackPointerEscapeDiagnostics(
            AnalysisResult& result, const std::vector<analysis::StackPointerEscapeIssue>& issues)
        {
            for (const auto& e : issues)
            {
                unsigned line = 0;
                unsigned column = 0;
                bool haveLoc = false;
                if (e.inst)
                {
                    llvm::DebugLoc DL = e.inst->getDebugLoc();
                    if (DL)
                    {
                        line = DL.getLine();
                        column = DL.getCol();
                        haveLoc = true;
                    }
                }

                std::ostringstream body;

                body << "  [!!] stack pointer escape: address of variable '" << e.varName
                     << "' escapes this function\n";

                if (e.escapeKind == "return")
                {
                    body << "       escape via return statement "
                            "(pointer to stack returned to caller)\n";
                }
                else if (e.escapeKind == "store_global")
                {
                    if (!e.targetName.empty())
                    {
                        body << "       stored into global variable '" << e.targetName
                             << "' (pointer may be used after the function returns)\n";
                    }
                    else
                    {
                        body << "       stored into a global variable "
                                "(pointer may be used after the function returns)\n";
                    }
                }
                else if (e.escapeKind == "store_unknown")
                {
                    body << "       stored through a non-local pointer "
                            "(e.g. via an out-parameter; pointer may outlive this function)\n";
                    if (!e.targetName.empty())
                    {
                        body << "       destination pointer/value name: '" << e.targetName << "'\n";
                    }
                }
                else if (e.escapeKind == "call_callback")
                {
                    body << "       address passed as argument to an indirect call "
                            "(callback may capture the pointer beyond this function)\n";
                }
                else if (e.escapeKind == "call_arg")
                {
                    if (!e.targetName.empty())
                    {
                        body << "       address passed as argument to function '" << e.targetName
                             << "' (callee may capture the pointer beyond this function)\n";
                    }
                    else
                    {
                        body << "       address passed as argument to a function "
                                "(callee may capture the pointer beyond this function)\n";
                    }
                }

                Diagnostic diag;
                diag.funcName = e.funcName;
                diag.line = haveLoc ? line : 0;
                diag.column = haveLoc ? column : 0;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::StackPointerEscape;
                diag.message = body.str();
                result.diagnostics.push_back(std::move(diag));
            }
        }

        static void
        appendConstParamDiagnostics(AnalysisResult& result,
                                    const std::vector<analysis::ConstParamIssue>& issues)
        {
            for (const auto& cp : issues)
            {
                std::ostringstream body;
                Diagnostic diag;
                std::string displayFuncName = analysis::formatFunctionNameForMessage(cp.funcName);

                diag.severity = DiagnosticSeverity::Info;
                diag.errCode = DescriptiveErrorCode::ConstParameterNotModified;

                const char* prefix = "[!]";
                if (diag.severity == DiagnosticSeverity::Warning)
                    prefix = "[!!]";
                else if (diag.severity == DiagnosticSeverity::Error)
                    prefix = "[!!!]";

                const char* subLabel = "Pointer";
                if (cp.pointerConstOnly)
                {
                    subLabel = "PointerConstOnly";
                }
                else if (cp.isReference)
                {
                    subLabel = cp.isRvalueRef ? "ReferenceRvaluePreferValue" : "Reference";
                }

                if (cp.isRvalueRef)
                {
                    body << "  " << prefix << "ConstParameterNotModified." << subLabel
                         << ": parameter '" << cp.paramName << "' in function '" << displayFuncName
                         << "' is an rvalue reference and is never used to modify the referred "
                            "object\n";
                    body << "       consider passing by value (" << cp.suggestedType
                         << ") or const reference (" << cp.suggestedTypeAlt << ")\n";
                    body << "       current type: " << cp.currentType << "\n";
                }
                else if (cp.pointerConstOnly)
                {
                    body << "  " << prefix << "ConstParameterNotModified." << subLabel
                         << ": parameter '" << cp.paramName << "' in function '" << displayFuncName
                         << "' is declared '" << cp.currentType
                         << "' but the pointed object is never modified\n";
                    body << "       consider '" << cp.suggestedType
                         << "' for API const-correctness\n";
                }
                else
                {
                    body << "  " << prefix << "ConstParameterNotModified." << subLabel
                         << ": parameter '" << cp.paramName << "' in function '" << displayFuncName
                         << "' is never used to modify the "
                         << (cp.isReference ? "referred" : "pointed") << " object\n";
                }

                if (!cp.isRvalueRef)
                {
                    body << "       current type: " << cp.currentType << "\n";
                    body << "       suggested type: " << cp.suggestedType << "\n";
                }

                diag.funcName = cp.funcName;
                diag.line = cp.line;
                diag.column = cp.column;
                diag.startLine = cp.line;
                diag.startColumn = cp.column;
                diag.endLine = cp.line;
                diag.endColumn = cp.column;
                diag.message = body.str();
                diag.ruleId = std::string("ConstParameterNotModified.") + subLabel;
                result.diagnostics.push_back(std::move(diag));
            }
        }
    } // namespace

    // ============================================================================
    // Types internes
    // ============================================================================

    // ============================================================================
    //  API publique : analyzeModule / analyzeFile
    // ============================================================================

    AnalysisResult analyzeModule(llvm::Module& mod, const AnalysisConfig& config)
    {
        using Clock = std::chrono::steady_clock;
        auto logDuration = [&](const char* label, Clock::time_point start)
        {
            if (!config.timing)
                return;
            auto end = Clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::cerr << label << " done in " << ms << " ms\n";
        };

        auto t0 = Clock::now();
        runFunctionAttrsPass(mod);
        logDuration("Function attrs pass", t0);

        t0 = Clock::now();
        ModuleAnalysisContext ctx = buildContext(mod, config);
        logDuration("Build context", t0);
        const llvm::DataLayout& DL = *ctx.dataLayout;
        auto shouldAnalyzeFunction = [&](const llvm::Function& F) -> bool
        { return ctx.shouldAnalyze(F); };

        // 1) Local stack per function
        t0 = Clock::now();
        LocalStackMap localStack = computeLocalStacks(ctx);
        logDuration("Compute local stacks", t0);

        // 2) Call graph
        t0 = Clock::now();
        analysis::CallGraph CG = buildCallGraphFiltered(ctx);
        logDuration("Build call graph", t0);

        // 3) Propagation + recursion detection
        t0 = Clock::now();
        analysis::InternalAnalysisState state = computeRecursionState(ctx, CG, localStack);
        logDuration("Compute recursion state", t0);

        // 4) Build public result
        FunctionAuxData aux;
        t0 = Clock::now();
        AnalysisResult result = buildResults(ctx, localStack, state, CG, aux);
        logDuration("Build results", t0);

        // 4b) Emit summary diagnostics for recursion/overflow flags (for JSON parity)
        t0 = Clock::now();
        emitSummaryDiagnostics(result, ctx, aux);
        logDuration("Emit summary diagnostics", t0);

        t0 = Clock::now();
        StackSize allocaLargeThreshold = analysis::computeAllocaLargeThreshold(config);
        logDuration("Compute alloca threshold", t0);

        // 6) Detect stack buffer overflows (intra-function analysis)
        t0 = Clock::now();
        std::vector<analysis::StackBufferOverflowIssue> bufferIssues =
            analysis::analyzeStackBufferOverflows(mod, shouldAnalyzeFunction);
        appendStackBufferDiagnostics(result, bufferIssues);
        logDuration("Stack buffer overflows", t0);

        // 8) Detect dynamic stack allocations (VLA / variable alloca)
        t0 = Clock::now();
        std::vector<analysis::DynamicAllocaIssue> dynAllocaIssues =
            analysis::analyzeDynamicAllocas(mod, shouldAnalyzeFunction);
        appendDynamicAllocaDiagnostics(result, dynAllocaIssues);
        logDuration("Dynamic allocas", t0);

        // 10) Analyze alloca usage (tainted / excessive size)
        t0 = Clock::now();
        std::vector<analysis::AllocaUsageIssue> allocaUsageIssues = analysis::analyzeAllocaUsage(
            mod, DL, state.RecursiveFuncs, state.InfiniteRecursionFuncs, shouldAnalyzeFunction);
        appendAllocaUsageDiagnostics(result, config, allocaLargeThreshold, allocaUsageIssues);
        logDuration("Alloca usage", t0);

        // 11) Detect overflows via memcpy/memset on stack buffers
        t0 = Clock::now();
        std::vector<analysis::MemIntrinsicIssue> memIssues =
            analysis::analyzeMemIntrinsicOverflows(mod, DL, shouldAnalyzeFunction);
        appendMemIntrinsicDiagnostics(result, memIssues);
        logDuration("Mem intrinsic overflows", t0);

        // 11b) Detect writes with "size-k" length
        t0 = Clock::now();
        std::vector<analysis::SizeMinusKWriteIssue> sizeMinusKIssues =
            analysis::analyzeSizeMinusKWrites(mod, DL, shouldAnalyzeFunction);
        appendSizeMinusKDiagnostics(result, sizeMinusKIssues);
        logDuration("Size-minus-k writes", t0);

        // 12) Detect multiple stores into the same stack buffer
        t0 = Clock::now();
        std::vector<analysis::MultipleStoreIssue> multiStoreIssues =
            analysis::analyzeMultipleStores(mod, shouldAnalyzeFunction);
        appendMultipleStoreDiagnostics(result, multiStoreIssues);

        // 12b) Détection de branches else-if inatteignables (condition dupliquée)
        std::vector<analysis::DuplicateIfConditionIssue> duplicateIfIssues =
            analysis::analyzeDuplicateIfConditions(mod, shouldAnalyzeFunction);
        appendDuplicateIfConditionDiagnostics(result, duplicateIfIssues);
        logDuration("Multiple stores", t0);

        // 13) Detect invalid base pointer reconstructions (offsetof/container_of)
        t0 = Clock::now();
        std::vector<analysis::InvalidBaseReconstructionIssue> baseReconIssues =
            analysis::analyzeInvalidBaseReconstructions(mod, DL, shouldAnalyzeFunction);
        appendInvalidBaseReconstructionDiagnostics(result, baseReconIssues);
        logDuration("Invalid base reconstructions", t0);

        // 14) Detect stack pointer escapes (potential use-after-return)
        t0 = Clock::now();
        std::vector<analysis::StackPointerEscapeIssue> escapeIssues =
            analysis::analyzeStackPointerEscapes(mod, shouldAnalyzeFunction);
        appendStackPointerEscapeDiagnostics(result, escapeIssues);
        logDuration("Stack pointer escapes", t0);

        // 15) Const-correctness: parameters that can be made const
        t0 = Clock::now();
        std::vector<analysis::ConstParamIssue> constParamIssues =
            analysis::analyzeConstParams(mod, shouldAnalyzeFunction);
        appendConstParamDiagnostics(result, constParamIssues);
        logDuration("Const params", t0);

        return result;
    }

    AnalysisResult analyzeFile(const std::string& filename, const AnalysisConfig& config,
                               llvm::LLVMContext& ctx, llvm::SMDiagnostic& err)
    {
        analysis::ModuleLoadResult load =
            analysis::loadModuleForAnalysis(filename, config, ctx, err);
        if (!load.module)
        {
            if (!load.error.empty())
                std::cerr << load.error;
            return AnalysisResult{config, {}};
        }

        using Clock = std::chrono::steady_clock;
        if (config.timing)
            std::cerr << "Analyzing " << filename << "...\n";
        auto analyzeStart = Clock::now();
        AnalysisResult result = analyzeModule(*load.module, config);
        if (config.timing)
        {
            auto analyzeEnd = Clock::now();
            auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(analyzeEnd - analyzeStart)
                    .count();
            std::cerr << "Analysis done in " << ms << " ms\n";
        }
        for (auto& f : result.functions)
        {
            if (f.filePath.empty())
                f.filePath = filename;
        }
        for (auto& d : result.diagnostics)
        {
            if (d.filePath.empty())
                d.filePath = filename;
        }
        return result;
    }

} // namespace ctrace::stack
