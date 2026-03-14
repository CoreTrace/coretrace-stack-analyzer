#include "analyzer/DiagnosticEmitter.hpp"

#include "analyzer/LocationResolver.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/Reachability.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <string_view>

#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        constexpr std::string_view kInfoPrefix = "[ !Info! ]";
        constexpr std::string_view kWarnPrefix = "[ !!Warn ]";
        constexpr std::string_view kErrorPrefix = "[!!!Error]";
        constexpr std::string_view kDiagIndentArrow = "\t\t ↳ ";

        constexpr std::string_view
        prefixForSeverity(ctrace::stack::DiagnosticSeverity severity) noexcept
        {
            switch (severity)
            {
            case ctrace::stack::DiagnosticSeverity::Info:
                return kInfoPrefix;
            case ctrace::stack::DiagnosticSeverity::Warning:
                return kWarnPrefix;
            case ctrace::stack::DiagnosticSeverity::Error:
                return kErrorPrefix;
            }
            return kWarnPrefix;
        }

        class DiagnosticBuilder
        {
          public:
            DiagnosticBuilder& function(std::string name)
            {
                diag_.funcName = std::move(name);
                return *this;
            }

            DiagnosticBuilder& filePath(std::string path)
            {
                diag_.filePath = std::move(path);
                return *this;
            }

            DiagnosticBuilder& severity(DiagnosticSeverity severity)
            {
                diag_.severity = severity;
                return *this;
            }

            DiagnosticBuilder& errCode(DescriptiveErrorCode code)
            {
                diag_.errCode = code;
                return *this;
            }

            DiagnosticBuilder& ruleId(std::string id)
            {
                diag_.ruleId = std::move(id);
                return *this;
            }

            DiagnosticBuilder& confidence(double value)
            {
                diag_.confidence = value;
                return *this;
            }

            DiagnosticBuilder& cwe(std::string id)
            {
                diag_.cweId = std::move(id);
                return *this;
            }

            DiagnosticBuilder& location(const ResolvedLocation& loc)
            {
                if (!loc.hasLocation)
                {
                    diag_.line = 0;
                    diag_.column = 0;
                    diag_.startLine = 0;
                    diag_.startColumn = 0;
                    diag_.endLine = 0;
                    diag_.endColumn = 0;
                    return *this;
                }

                diag_.line = loc.line;
                diag_.column = loc.column;
                diag_.startLine = loc.startLine;
                diag_.startColumn = loc.startColumn;
                diag_.endLine = loc.endLine;
                diag_.endColumn = loc.endColumn;
                return *this;
            }

            DiagnosticBuilder& lineColumn(unsigned line, unsigned column)
            {
                diag_.line = line;
                diag_.column = column;
                diag_.startLine = line;
                diag_.startColumn = column;
                diag_.endLine = line;
                diag_.endColumn = column;
                return *this;
            }

            DiagnosticBuilder& message(std::string text)
            {
                diag_.message = std::move(text);
                return *this;
            }

            DiagnosticBuilder& variableAliasing(std::vector<std::string> aliasing)
            {
                diag_.variableAliasingVec = std::move(aliasing);
                return *this;
            }

            Diagnostic build()
            {
                return std::move(diag_);
            }

          private:
            Diagnostic diag_;
        };
    } // namespace

    AnalysisResult buildResults(const PreparedModule& prepared, FunctionAuxData& aux)
    {
        AnalysisResult result;
        result.config = prepared.ctx.config;

        for (llvm::Function* function : prepared.ctx.functions)
        {
            const llvm::Function* fn = function;

            analysis::LocalStackInfo localInfo;
            analysis::StackEstimate totalInfo;

            if (auto itLocal = prepared.localStack.find(fn); itLocal != prepared.localStack.end())
                localInfo = itLocal->second;

            if (auto itTotal = prepared.recursionState.TotalStack.find(fn);
                itTotal != prepared.recursionState.TotalStack.end())
            {
                totalInfo = itTotal->second;
            }

            FunctionResult functionResult;
            functionResult.name = function->getName().str();
            functionResult.filePath = analysis::getFunctionSourcePath(*function);
            if (functionResult.filePath.empty() && !prepared.ctx.filter.moduleSourcePath.empty())
                functionResult.filePath = prepared.ctx.filter.moduleSourcePath;
            functionResult.localStack = localInfo.bytes;
            functionResult.localStackUnknown = localInfo.unknown;
            functionResult.maxStack = totalInfo.bytes;
            functionResult.maxStackUnknown = totalInfo.unknown;
            functionResult.hasDynamicAlloca = localInfo.hasDynamicAlloca;
            functionResult.isRecursive = prepared.recursionState.RecursiveFuncs.count(fn) != 0;
            functionResult.hasInfiniteSelfRecursion =
                prepared.recursionState.InfiniteRecursionFuncs.count(fn) != 0;
            functionResult.exceedsLimit = (!functionResult.maxStackUnknown &&
                                           totalInfo.bytes > prepared.ctx.config.stackLimit);

            unsigned line = 0;
            unsigned column = 0;
            if (analysis::getFunctionSourceLocation(*function, line, column))
                aux.locations[fn] = {line, column};

            if (!functionResult.isRecursive && totalInfo.bytes > localInfo.bytes)
            {
                std::string path = analysis::buildMaxStackCallPath(fn, prepared.callGraph,
                                                                   prepared.recursionState);
                if (!path.empty())
                    aux.callPaths[fn] = path;
            }
            if (!localInfo.localAllocas.empty())
                aux.localAllocas[fn] = localInfo.localAllocas;

            result.functions.push_back(std::move(functionResult));
            aux.indices[fn] = result.functions.size() - 1;
        }

        return result;
    }

    void emitSummaryDiagnostics(AnalysisResult& result, const PreparedModule& prepared,
                                const FunctionAuxData& aux)
    {
        for (const llvm::Function* function : prepared.ctx.functions)
        {
            const auto itIndex = aux.indices.find(function);
            if (itIndex == aux.indices.end())
                continue;

            const std::size_t index = itIndex->second;
            if (index >= result.functions.size())
                continue;

            const FunctionResult& functionResult = result.functions[index];
            SourceLocation functionLoc{};
            bool hasFunctionLoc = false;
            if (const auto itLoc = aux.locations.find(function); itLoc != aux.locations.end())
            {
                functionLoc = itLoc->second;
                hasFunctionLoc = (functionLoc.line != 0);
            }

            if (functionResult.isRecursive)
            {
                DiagnosticBuilder builder;
                builder.function(functionResult.name)
                    .filePath(functionResult.filePath)
                    .severity(DiagnosticSeverity::Info)
                    .errCode(DescriptiveErrorCode::None)
                    .message("\t" + std::string(prefixForSeverity(DiagnosticSeverity::Info)) +
                             " recursive or mutually recursive function detected\n");
                if (hasFunctionLoc)
                    builder.lineColumn(functionLoc.line, functionLoc.column);
                result.diagnostics.push_back(builder.build());
            }

            if (functionResult.hasInfiniteSelfRecursion)
            {
                DiagnosticBuilder builder;
                builder.function(functionResult.name)
                    .filePath(functionResult.filePath)
                    .severity(DiagnosticSeverity::Error)
                    .errCode(DescriptiveErrorCode::None)
                    .message("\t" + std::string(prefixForSeverity(DiagnosticSeverity::Error)) +
                             " unconditional self recursion detected (no base case)\n"
                             "\t\t ↳ this will eventually overflow the stack at runtime\n");
                if (hasFunctionLoc)
                    builder.lineColumn(functionLoc.line, functionLoc.column);
                result.diagnostics.push_back(builder.build());
            }

            if (!functionResult.exceedsLimit)
                continue;

            DiagnosticBuilder builder;
            builder.function(functionResult.name)
                .filePath(functionResult.filePath)
                .severity(DiagnosticSeverity::Error)
                .errCode(DescriptiveErrorCode::StackFrameTooLarge);
            if (hasFunctionLoc)
                builder.lineColumn(functionLoc.line, functionLoc.column);

            std::string message;
            bool suppressLocation = false;
            const StackSize maxCallee = (functionResult.maxStack > functionResult.localStack)
                                            ? (functionResult.maxStack - functionResult.localStack)
                                            : 0;

            if (const auto itLocals = aux.localAllocas.find(function);
                functionResult.localStack >= maxCallee && itLocals != aux.localAllocas.end())
            {
                std::string localsDetails;
                std::string singleName;
                StackSize singleSize = 0;
                for (const auto& entry : itLocals->second)
                {
                    if (entry.first == "<unnamed>")
                        continue;
                    if (entry.second >= prepared.ctx.config.stackLimit && entry.second > singleSize)
                    {
                        singleName = entry.first;
                        singleSize = entry.second;
                    }
                }

                std::string aliasLine;
                if (!singleName.empty())
                {
                    aliasLine = "\t\t ↳ alias variable: " + singleName + "\n";
                }
                else if (!itLocals->second.empty())
                {
                    localsDetails += "\t\t ↳ locals: " + std::to_string(itLocals->second.size()) +
                                     " variables (total " +
                                     std::to_string(functionResult.localStack) + " bytes)\n";

                    std::vector<std::pair<std::string, StackSize>> named = itLocals->second;
                    named.erase(std::remove_if(named.begin(), named.end(), [](const auto& value)
                                               { return value.first == "<unnamed>"; }),
                                named.end());
                    std::sort(named.begin(), named.end(),
                              [](const auto& lhs, const auto& rhs)
                              {
                                  if (lhs.second != rhs.second)
                                      return lhs.second > rhs.second;
                                  return lhs.first < rhs.first;
                              });

                    if (!named.empty())
                    {
                        constexpr std::size_t kMaxLocalsForLocation = 5;
                        if (named.size() > kMaxLocalsForLocation)
                            suppressLocation = true;

                        std::string listLine = "        locals list: ";
                        for (std::size_t i = 0; i < named.size(); ++i)
                        {
                            if (i > 0)
                                listLine += ", ";
                            listLine +=
                                named[i].first + "(" + std::to_string(named[i].second) + ")";
                        }
                        localsDetails += listLine + "\n";
                    }
                }

                if (!localsDetails.empty())
                    message += localsDetails;
                message = aliasLine + message;
            }

            std::string suffix;
            if (const auto itPath = aux.callPaths.find(function); itPath != aux.callPaths.end())
            {
                suffix += "\t\t ↳ path: " + itPath->second + "\n";
            }

            const std::string mainLine = " potential stack overflow: exceeds limit of " +
                                         std::to_string(prepared.ctx.config.stackLimit) +
                                         " bytes\n";

            message = "\t" + std::string(prefixForSeverity(DiagnosticSeverity::Error)) + mainLine +
                      message + suffix;

            if (suppressLocation)
                builder.lineColumn(0, 0);

            builder.message(std::move(message));
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendStackBufferDiagnostics(
        AnalysisResult& result, const std::vector<analysis::StackBufferOverflowIssue>& bufferIssues)
    {
        for (const auto& issue : bufferIssues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);
            const bool isUnreachable = analysis::isStaticallyUnreachableStackAccess(issue);

            std::ostringstream body;
            DiagnosticBuilder builder;

            if (issue.isLowerBoundViolation)
            {
                builder.errCode(DescriptiveErrorCode::NegativeStackIndex);
                body << "  [!!] potential negative index on variable '" << issue.varName
                     << "' (size " << issue.arraySize << ")\n";
                if (!issue.aliasPath.empty())
                    body << "\t\t ↳ alias path: " << issue.aliasPath << "\n";
                body << "\t\t ↳ inferred lower bound for index expression: " << issue.lowerBound
                     << " (index may be < 0)\n";
            }
            else
            {
                builder.errCode(DescriptiveErrorCode::StackBufferOverflow);
                const bool isGlobalStorage =
                    issue.storageClass == analysis::BufferStorageClass::Global;
                body << "\t[ !!Warn ] potential "
                     << (isGlobalStorage ? "buffer overflow on global variable '"
                                         : "stack buffer overflow on variable '")
                     << issue.varName << "' (size " << issue.arraySize << ")\n";
                if (!issue.aliasPath.empty())
                    body << "\t\t ↳ alias path: " << issue.aliasPath << "\n";
                if (issue.indexIsConstant)
                {
                    body << "\t\t ↳ constant index " << issue.indexOrUpperBound
                         << " is out of bounds (0.." << (issue.arraySize ? issue.arraySize - 1 : 0)
                         << ")\n";
                }
                else
                {
                    body << "\t\t ↳ index variable may go up to " << issue.indexOrUpperBound
                         << " (array last valid index: "
                         << (issue.arraySize ? issue.arraySize - 1 : 0) << ")\n";
                }
            }

            if (issue.isWrite)
                body << "\t\t ↳ (this is a write access)\n";
            else
                body << "\t\t ↳ (this is a read access)\n";

            if (isUnreachable)
            {
                body << "\t\t ↳ [info] this access appears unreachable at runtime "
                        "(condition is always false for this branch)\n";
            }

            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .location(loc)
                .message(body.str())
                .variableAliasing(issue.aliasPathVec);

            result.diagnostics.push_back(builder.build());
        }
    }

    void appendDynamicAllocaDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::DynamicAllocaIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc =
                resolveFromInstruction(static_cast<const llvm::Instruction*>(issue.allocaInst));

            std::ostringstream body;
            body << "\t[ !!Warn ] dynamic stack allocation detected for variable '" << issue.varName
                 << "'\n";
            body << "\t\t ↳ allocated type: " << issue.typeName << "\n";
            body << "\t\t ↳ size of this allocation is not compile-time constant "
                    "(VLA / variable alloca) and may lead to unbounded stack usage\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::VLAUsage)
                .location(loc)
                .message(body.str());

            result.diagnostics.push_back(builder.build());
        }
    }

    void appendAllocaUsageDiagnostics(AnalysisResult& result, const AnalysisConfig& config,
                                      StackSize allocaLargeThreshold,
                                      const std::vector<analysis::AllocaUsageIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc =
                resolveFromInstruction(static_cast<const llvm::Instruction*>(issue.allocaInst));

            bool isOversized = false;
            if (issue.sizeIsConst && issue.sizeBytes >= allocaLargeThreshold)
                isOversized = true;
            else if (issue.hasUpperBound && issue.upperBoundBytes >= allocaLargeThreshold)
                isOversized = true;
            else if (issue.sizeIsConst && config.stackLimit != 0 &&
                     issue.sizeBytes >= config.stackLimit)
                isOversized = true;

            std::ostringstream body;
            DiagnosticBuilder builder;
            builder.function(issue.funcName).location(loc);

            if (isOversized)
            {
                builder.severity(DiagnosticSeverity::Error)
                    .errCode(DescriptiveErrorCode::AllocaTooLarge);
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Error)
                     << " large alloca on the stack for variable '" << issue.varName << "'\n";
            }
            else if (issue.userControlled)
            {
                builder.severity(DiagnosticSeverity::Warning)
                    .errCode(DescriptiveErrorCode::AllocaUserControlled);
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " user-controlled alloca size for variable '" << issue.varName << "'\n";
            }
            else
            {
                builder.severity(DiagnosticSeverity::Warning)
                    .errCode(DescriptiveErrorCode::AllocaUsageWarning);
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " dynamic alloca on the stack for variable '" << issue.varName << "'\n";
            }

            body << "\t\t ↳ allocation performed via alloca/VLA; stack usage grows with runtime "
                    "value\n";

            if (issue.sizeIsConst)
                body << "\t\t ↳ requested stack size: " << issue.sizeBytes << " bytes\n";
            else if (issue.hasUpperBound)
                body << "\t\t ↳ inferred upper bound for size: " << issue.upperBoundBytes
                     << " bytes\n";
            else
                body << "\t\t ↳ size is unbounded at compile time\n";

            if (issue.isInfiniteRecursive)
            {
                builder.severity(DiagnosticSeverity::Error);
                body << "\t\t ↳ function is infinitely recursive; this alloca runs at every "
                        "frame and guarantees stack overflow\n";
            }
            else if (issue.isRecursive)
            {
                if (isOversized || issue.userControlled)
                    builder.severity(DiagnosticSeverity::Error);
                body << "\t\t ↳ function is recursive; this allocation repeats at each "
                        "recursion depth and can exhaust the stack\n";
            }

            if (isOversized)
            {
                body << "\t\t ↳ exceeds safety threshold of " << allocaLargeThreshold << " bytes";
                if (config.stackLimit != 0)
                    body << " (stack limit: " << config.stackLimit << " bytes)";
                body << "\n";
            }
            else if (issue.userControlled)
            {
                body << "\t\t ↳ size depends on user-controlled input "
                        "(function argument or non-local value)\n";
            }
            else
            {
                body << "\t\t ↳ size does not appear user-controlled but remains "
                        "runtime-dependent\n";
            }

            builder.message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendMemIntrinsicDiagnostics(AnalysisResult& result,
                                       const std::vector<analysis::MemIntrinsicIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " potential stack buffer overflow in " << issue.intrinsicName
                 << " on variable '" << issue.varName << "'\n";
            body << "\t\t ↳ destination stack buffer size: " << issue.destSizeBytes << " bytes\n";
            if (issue.hasExplicitLength)
            {
                body << "\t\t ↳ requested " << issue.lengthBytes
                     << " bytes to be copied/initialized\n";
            }
            else
            {
                body << "\t\t ↳ this API has no explicit size argument; "
                        "destination fit cannot be proven statically\n";
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendSizeMinusKDiagnostics(AnalysisResult& result,
                                     const std::vector<analysis::SizeMinusKWriteIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst);

            std::ostringstream body;
            if (issue.hasPointerDest)
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential unsafe write with length (size - " << issue.k << ")";
            }
            else
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential unsafe size-" << issue.k << " argument passed";
            }
            if (!issue.sinkName.empty())
                body << " in " << issue.sinkName;
            body << "\n";
            if (issue.hasPointerDest && !issue.ptrNonNull)
                body << "\t\t ↳ destination pointer may be null\n";
            if (!issue.sizeAboveK)
                body << "\t\t ↳ size operand may be <= " << issue.k << "\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::SizeMinusOneWrite)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendIntegerOverflowDiagnostics(AnalysisResult& result,
                                          const std::vector<analysis::IntegerOverflowIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::IntegerOverflow)
                .location(loc)
                .confidence(0.70);

            std::ostringstream body;
            switch (issue.kind)
            {
            case analysis::IntegerOverflowIssueKind::ArithmeticInSizeComputation:
                builder.ruleId("IntegerOverflow.SizeComputation").cwe("CWE-190");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential integer overflow in size computation before '" << issue.sinkName
                     << "'\n";
                body << kDiagIndentArrow << "operation: " << issue.operation << "\n";
                body << kDiagIndentArrow
                     << "overflowed size may under-allocate memory or make bounds checks unsound\n";
                break;
            case analysis::IntegerOverflowIssueKind::SignedToUnsignedSize:
                builder.ruleId("IntegerConversion.SignedToSize").cwe("CWE-195");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential signed-to-size conversion before '" << issue.sinkName << "'\n";
                body << kDiagIndentArrow
                     << "a possibly negative signed value is converted to an unsigned length\n";
                body
                    << kDiagIndentArrow
                    << "this can become a very large size value and trigger out-of-bounds access\n";
                break;
            case analysis::IntegerOverflowIssueKind::TruncationInSizeComputation:
                builder.ruleId("IntegerTruncation.SizeComputation").cwe("CWE-197");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential integer truncation in size computation before '"
                     << issue.sinkName << "'\n";
                body << kDiagIndentArrow
                     << "narrowing conversion may drop high bits and produce a smaller buffer "
                        "size\n";
                break;
            case analysis::IntegerOverflowIssueKind::SignedArithmeticOverflow:
                builder.ruleId("IntegerOverflow.SignedArithmetic").cwe("CWE-190");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential signed integer overflow in arithmetic operation\n";
                body << kDiagIndentArrow << "operation: " << issue.operation << "\n";
                body << kDiagIndentArrow
                     << "result is returned without a provable non-overflow bound\n";
                break;
            }

            builder.message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendMultipleStoreDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::MultipleStoreIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            unsigned line = 0;
            unsigned column = 0;
            const bool haveLoc = resolveAllocaSourceLocation(issue.allocaInst, line, column);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                 << " multiple stores to stack buffer '" << issue.varName << "' in this function ("
                 << issue.storeCount << " store instruction(s)";
            if (issue.distinctIndexCount > 0)
                body << ", " << issue.distinctIndexCount << " distinct index expression(s)";
            body << ")\n";

            if (issue.distinctIndexCount == 1)
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                     << " all stores use the same index expression "
                        "(possible redundant or unintended overwrite)\n";
            }
            else if (issue.distinctIndexCount > 1)
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                     << " stores use different index expressions; verify indices are "
                        "correct and non-overlapping\n";
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Info)
                .errCode(DescriptiveErrorCode::MultipleStoresToStackBuffer)
                .message(body.str());
            if (haveLoc)
                builder.lineColumn(line, column);
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendDuplicateIfConditionDiagnostics(
        AnalysisResult& result, const std::vector<analysis::DuplicateIfConditionIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.conditionInst, true);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " unreachable else-if branch: condition is equivalent to a previous "
                    "'if' condition\n";
            body << "\t\t ↳ else branch implies previous condition is false\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::DuplicateIfCondition)
                .ruleId("DuplicateIfCondition")
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendUninitializedLocalReadDiagnostics(
        AnalysisResult& result, const std::vector<analysis::UninitializedLocalReadIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            unsigned line = issue.line;
            unsigned column = issue.column;
            bool haveLoc = (line != 0);

            const ResolvedLocation locFromInst = resolveFromInstruction(issue.inst);
            if (locFromInst.hasLocation)
            {
                line = locFromInst.line;
                column = locFromInst.column;
                haveLoc = true;
            }

            std::ostringstream body;
            std::string ruleId = "UninitializedLocalRead";
            std::string cwe = "CWE-457";
            double confidence = 0.90;
            if (issue.kind == analysis::UninitializedLocalIssueKind::ReadBeforeDefiniteInit)
            {
                body << "\t[ !!Warn ] potential read of uninitialized local variable '"
                     << issue.varName << "'\n";
                body << "\t\t ↳ this load may execute before any definite initialization on "
                        "all control-flow paths\n";
            }
            else if (issue.kind ==
                     analysis::UninitializedLocalIssueKind::ReadBeforeDefiniteInitViaCall)
            {
                body << "\t[ !!Warn ] potential read of uninitialized local variable '"
                     << issue.varName << "'\n";
                body << "\t\t ↳ this call may read the value before any definite initialization";
                if (!issue.calleeName.empty())
                    body << " in '" << issue.calleeName << "'";
                body << "\n";
            }
            else if (issue.kind ==
                     analysis::UninitializedLocalIssueKind::ExposedUninitializedBytesViaSink)
            {
                ruleId = "InformationExposure.UninitializedStackBytes";
                cwe = "CWE-200";
                confidence = 0.80;
                body << "\t[ !!Warn ] potential information leak: local variable '" << issue.varName
                     << "' may expose uninitialized bytes through external sink";
                if (!issue.calleeName.empty())
                    body << " '" << issue.calleeName << "'";
                body << "\n";
                body << "\t\t ↳ transmitted range is not fully initialized on all control-flow "
                        "paths\n";
            }
            else
            {
                ruleId = "UninitializedLocalVariable";
                confidence = 0.75;
                body << "\t[ !!Warn ] local variable '" << issue.varName
                     << "' is never initialized\n";
                body << "\t\t ↳ declared without initializer and no definite write was found "
                        "in this function\n";
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::UninitializedLocalRead)
                .message(body.str());

            if (haveLoc)
                builder.lineColumn(line, column);

            builder.ruleId(ruleId).confidence(confidence).cwe(cwe);

            result.diagnostics.push_back(builder.build());
        }
    }

    void appendGlobalReadBeforeWriteDiagnostics(
        AnalysisResult& result, const std::vector<analysis::GlobalReadBeforeWriteIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation readLoc = resolveFromInstruction(issue.readInst, true);
            const ResolvedLocation firstWriteLoc =
                resolveFromInstruction(issue.firstWriteInst, true);

            std::ostringstream body;
            body << "\t[ !!Warn ] potential read of global buffer '" << issue.globalName
                 << "' before first write in this function\n";
            body << "\t\t ↳ this buffer has static zero initialization; read is defined but may "
                    "indicate stale/default-state use\n";
            double confidence = 0.60;
            if (firstWriteLoc.hasLocation)
            {
                body << "\t\t ↳ first write appears later at line " << firstWriteLoc.line
                     << ", column " << firstWriteLoc.column << "\n";
            }
            else if (issue.kind == analysis::GlobalReadBeforeWriteKind::WithoutLocalWrite)
            {
                body << "\t\t ↳ no write to this buffer is observed in this function; value may "
                        "come from static initialization or writes in other functions/TUs\n";
                confidence = issue.hasNonLocalWrite ? 0.50 : 0.55;
                if (issue.hasNonLocalWrite)
                {
                    body << "\t\t ↳ at least one write to this buffer is observed in another "
                            "analyzed function/TU\n";
                }
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::GlobalReadBeforeWrite)
                .ruleId("GlobalReadBeforeWrite")
                .cwe("CWE-665")
                .confidence(confidence)
                .location(readLoc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendInvalidBaseReconstructionDiagnostics(
        AnalysisResult& result, const std::vector<analysis::InvalidBaseReconstructionIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            std::ostringstream body;
            body << "\t[ !!Warn ] potential UB: invalid base reconstruction via "
                    "offsetof/container_of\n";
            body << "\t\t ↳ variable: '" << issue.varName << "'\n";
            body << "\t\t ↳ source member: " << issue.sourceMember << "\n";
            body << "\t\t ↳ offset applied: " << (issue.offsetUsed >= 0 ? "+" : "")
                 << issue.offsetUsed << " bytes\n";
            body << "\t\t ↳ target type: " << issue.targetType << "\n";

            DiagnosticSeverity severity = DiagnosticSeverity::Warning;
            if (issue.isOutOfBounds)
            {
                severity = DiagnosticSeverity::Error;
                body << "\t[!!!Error] derived pointer points OUTSIDE the valid object range\n";
                body << "\t\t ↳ (this will cause undefined behavior if dereferenced)\n";
            }
            else
            {
                body << "\t[ !!Warn ] unable to verify that derived pointer points to a "
                        "valid object\n";
                body << "\t\t ↳ (potential undefined behavior if offset is incorrect)\n";
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(severity)
                .errCode(DescriptiveErrorCode::InvalidBaseReconstruction)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendStackPointerEscapeDiagnostics(
        AnalysisResult& result, const std::vector<analysis::StackPointerEscapeIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " stack pointer escape: address of variable '" << issue.varName
                 << "' escapes this function\n";

            if (issue.escapeKind == "return")
            {
                body << "\t\t ↳ escape via return statement "
                        "(pointer to stack returned to caller)\n";
            }
            else if (issue.escapeKind == "store_global")
            {
                if (!issue.targetName.empty())
                {
                    body << "\t\t ↳ stored into global variable '" << issue.targetName
                         << "' (pointer may be used after the function returns)\n";
                }
                else
                {
                    body << "\t\t ↳ stored into a global variable "
                            "(pointer may be used after the function returns)\n";
                }
            }
            else if (issue.escapeKind == "store_unknown")
            {
                body << "\t\t ↳ stored through a non-local pointer "
                        "(e.g. via an out-parameter; pointer may outlive this function)\n";
                if (!issue.targetName.empty())
                    body << "\t\t ↳ destination pointer/value name: '" << issue.targetName << "'\n";
            }
            else if (issue.escapeKind == "call_callback")
            {
                body << "\t\t ↳ address passed as argument to an indirect call "
                        "(callback may capture the pointer beyond this function)\n";
            }
            else if (issue.escapeKind == "call_arg")
            {
                if (!issue.targetName.empty())
                {
                    body << "\t\t ↳ address passed as argument to function '" << issue.targetName
                         << "' (callee may capture the pointer beyond this function)\n";
                }
                else
                {
                    body << "\t\t ↳ address passed as argument to a function "
                            "(callee may capture the pointer beyond this function)\n";
                }
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::StackPointerEscape)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendConstParamDiagnostics(AnalysisResult& result,
                                     const std::vector<analysis::ConstParamIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            std::ostringstream body;
            const std::string displayFuncName =
                analysis::formatFunctionNameForMessage(issue.funcName);
            const std::string& paramName = issue.binding.name;

            const char* subLabel = "Pointer";
            if (issue.pointerConstOnly)
                subLabel = "PointerConstOnly";
            else if (issue.isReference)
                subLabel = issue.isRvalueRef ? "ReferenceRvaluePreferValue" : "Reference";

            if (issue.isRvalueRef)
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                     << " ConstParameterNotModified." << subLabel << ": parameter '" << paramName
                     << "' in function '" << displayFuncName
                     << "' is an rvalue reference and is never used to modify the referred "
                        "object\n";
                body << kDiagIndentArrow << "consider passing by value (" << issue.suggestedType
                     << ") or const reference (" << issue.suggestedTypeAlt << ")\n";
                body << kDiagIndentArrow << "current type: " << issue.currentType << "\n";
            }
            else if (issue.pointerConstOnly)
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                     << " ConstParameterNotModified." << subLabel << ": parameter '" << paramName
                     << "' in function '" << displayFuncName << "' is declared '"
                     << issue.currentType << "' but the pointed object is never modified\n";
                body << kDiagIndentArrow << "consider '" << issue.suggestedType
                     << "' for API const-correctness\n";
            }
            else
            {
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Info)
                     << " ConstParameterNotModified." << subLabel << ": parameter '" << paramName
                     << "' in function '" << displayFuncName << "' is never used to modify the "
                     << (issue.isReference ? "referred" : "pointed") << " object\n";
            }

            if (!issue.isRvalueRef)
            {
                body << kDiagIndentArrow << "current type: " << issue.currentType << "\n";
                body << kDiagIndentArrow << "suggested type: " << issue.suggestedType << "\n";
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .severity(DiagnosticSeverity::Info)
                .errCode(DescriptiveErrorCode::ConstParameterNotModified)
                .lineColumn(issue.binding.line, issue.binding.column)
                .ruleId(std::string("ConstParameterNotModified.") + subLabel)
                .message(body.str());
            if (issue.confidence >= 0.0)
                builder.confidence(issue.confidence);
            result.diagnostics.push_back(builder.build());
        }
    }

    void
    appendCommandInjectionDiagnostics(AnalysisResult& result,
                                      const std::vector<analysis::CommandInjectionIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " potential command injection: non-literal command reaches '" << issue.sinkName
                 << "'\n";
            body << kDiagIndentArrow
                 << "the command argument is not a compile-time string literal\n";
            body << kDiagIndentArrow
                 << "validate/sanitize external input or avoid shell command composition\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::CommandInjection)
                .ruleId("CommandInjection.NonLiteralCommand")
                .cwe("CWE-78")
                .confidence(0.70)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendTOCTOUDiagnostics(AnalysisResult& result,
                                 const std::vector<analysis::TOCTOUIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " potential TOCTOU race: path checked with '" << issue.checkApi
                 << "' then used with '" << issue.useApi << "'\n";
            body << kDiagIndentArrow
                 << "the file target may change between check and use operations\n";
            body << kDiagIndentArrow
                 << "prefer descriptor-based validation (open + fstat) on the same handle\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::TOCTOURace)
                .ruleId("TOCTOU.PathCheckThenUse")
                .cwe("CWE-367")
                .confidence(0.70)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendNullDerefDiagnostics(AnalysisResult& result,
                                    const std::vector<analysis::NullDerefIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            DiagnosticSeverity severity = DiagnosticSeverity::Error;
            double confidence = 0.75;
            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Error)
                 << " potential null pointer dereference on '" << issue.pointerName << "'\n";

            switch (issue.kind)
            {
            case analysis::NullDerefIssueKind::DirectNullPointer:
                body << kDiagIndentArrow
                     << "the dereferenced pointer is directly null at this instruction\n";
                break;
            case analysis::NullDerefIssueKind::NullBranchDereference:
                body << kDiagIndentArrow
                     << "control flow proves pointer is null on this branch before dereference\n";
                break;
            case analysis::NullDerefIssueKind::NullStoredInLocalSlot:
                body << kDiagIndentArrow
                     << "a preceding local-slot store sets the pointer to null before use\n";
                break;
            case analysis::NullDerefIssueKind::UncheckedAllocatorResult:
                severity = DiagnosticSeverity::Warning;
                confidence = 0.70;
                body.str("");
                body.clear();
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential null pointer dereference on '" << issue.pointerName << "'\n";
                body << kDiagIndentArrow
                     << "pointer comes from allocator return value and is dereferenced without a "
                        "provable null-check\n";
                break;
            }

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(severity)
                .errCode(DescriptiveErrorCode::NullPointerDereference)
                .ruleId("NullPointerDereference")
                .cwe("CWE-476")
                .confidence(confidence)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendTypeConfusionDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::TypeConfusionIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            std::ostringstream body;
            body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                 << " potential type confusion: incompatible struct views on the same pointer\n";
            body << kDiagIndentArrow << "smaller observed view: '" << issue.smallerViewType << "' ("
                 << issue.smallerViewSizeBytes << " bytes)\n";
            body << kDiagIndentArrow << "accessed view: '" << issue.accessedViewType
                 << "' at byte offset " << issue.accessOffsetBytes << "\n";
            body << kDiagIndentArrow
                 << "field access may read/write outside the actual object layout\n";

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::TypeConfusion)
                .ruleId("TypeConfusion.IncompatibleStructView")
                .cwe("CWE-843")
                .confidence(0.65)
                .location(loc)
                .message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void appendOOBReadDiagnostics(AnalysisResult& result,
                                  const std::vector<analysis::OOBReadIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst, true);

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .filePath(issue.filePath)
                .severity(DiagnosticSeverity::Warning)
                .errCode(DescriptiveErrorCode::OutOfBoundsRead)
                .location(loc)
                .confidence(0.70)
                .cwe("CWE-125");

            std::ostringstream body;
            if (issue.kind == analysis::OOBReadIssueKind::MissingNullTerminator)
            {
                builder.ruleId("OutOfBoundsRead.MissingNullTerminator");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential out-of-bounds read: string buffer '" << issue.bufferName
                     << "' may be missing a null terminator before '" << issue.apiName << "'\n";
                body << kDiagIndentArrow << "buffer size: " << issue.bufferSizeBytes
                     << " bytes, last write size: " << issue.writeSizeBytes << " bytes\n";
                body << kDiagIndentArrow
                     << "unterminated strings can make read APIs scan past buffer bounds\n";
            }
            else
            {
                builder.ruleId("OutOfBoundsRead.HeapIndex");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential out-of-bounds read on heap buffer '" << issue.bufferName
                     << "' via unchecked index\n";
                body << kDiagIndentArrow << "inferred heap capacity: " << issue.capacityElements
                     << " element(s)\n";
                body << kDiagIndentArrow
                     << "index value is not proven to be within [0, capacity-1]\n";
            }

            builder.message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

    void
    appendResourceLifetimeDiagnostics(AnalysisResult& result,
                                      const std::vector<analysis::ResourceLifetimeIssue>& issues)
    {
        for (const auto& issue : issues)
        {
            const ResolvedLocation loc = resolveFromInstruction(issue.inst);

            DiagnosticBuilder builder;
            builder.function(issue.funcName)
                .errCode(DescriptiveErrorCode::ResourceLifetimeIssue)
                .confidence(0.80)
                .location(loc);

            std::ostringstream body;
            switch (issue.kind)
            {
            case analysis::ResourceLifetimeIssueKind::MissingRelease:
                builder.severity(DiagnosticSeverity::Warning)
                    .ruleId("ResourceLifetime.MissingRelease")
                    .cwe("CWE-772");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " potential resource leak: '" << issue.resourceKind
                     << "' acquired in handle '" << issue.handleName
                     << "' is not released in this function\n";
                body << kDiagIndentArrow
                     << "no matching release call was found for the tracked handle\n";
                break;
            case analysis::ResourceLifetimeIssueKind::DoubleRelease:
                builder.severity(DiagnosticSeverity::Error)
                    .ruleId("ResourceLifetime.DoubleRelease")
                    .cwe("CWE-415");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Error)
                     << " potential double release: '" << issue.resourceKind << "' handle '"
                     << issue.handleName
                     << "' is released without a matching acquire in this function\n";
                body << kDiagIndentArrow
                     << "this may indicate release-after-release or ownership mismatch\n";
                break;
            case analysis::ResourceLifetimeIssueKind::MissingDestructorRelease:
                builder.severity(DiagnosticSeverity::Warning)
                    .ruleId("ResourceLifetime.MissingDestructorRelease")
                    .cwe("CWE-772");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " resource acquired in constructor may leak: class '" << issue.className
                     << "' does not release '" << issue.resourceKind << "' field '"
                     << issue.handleName << "' in destructor\n";
                body << kDiagIndentArrow
                     << "tracked constructor acquisitions for this field have no matching "
                        "destructor release\n";
                break;
            case analysis::ResourceLifetimeIssueKind::IncompleteInterproc:
                builder.severity(DiagnosticSeverity::Warning)
                    .ruleId("ResourceLifetime.IncompleteInterproc");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " inter-procedural resource analysis incomplete: handle '"
                     << issue.handleName
                     << "' may be acquired by an unmodeled/external callee before release\n";
                body << kDiagIndentArrow
                     << "no matching resource model rule or cross-TU summary was found for at "
                        "least one related call\n";
                body << kDiagIndentArrow
                     << "include callee definitions in inputs or extend --resource-model to "
                        "improve precision\n";
                break;
            case analysis::ResourceLifetimeIssueKind::UseAfterRelease:
                builder.severity(DiagnosticSeverity::Error)
                    .ruleId("ResourceLifetime.UseAfterRelease")
                    .cwe("CWE-416");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Error)
                     << " potential use-after-release: '" << issue.resourceKind << "' handle '"
                     << issue.handleName << "' is used after a release in this function\n";
                body << kDiagIndentArrow
                     << "a later dereference/call argument use may access invalid memory\n";
                break;
            case analysis::ResourceLifetimeIssueKind::ReleasedHandleEscapes:
                builder.severity(DiagnosticSeverity::Warning)
                    .ruleId("ResourceLifetime.ReleasedHandleEscapes")
                    .cwe("CWE-416");
                body << "\t" << prefixForSeverity(DiagnosticSeverity::Warning)
                     << " released handle derived from '" << issue.handleName
                     << "' may escape through a returned owner object\n";
                body << kDiagIndentArrow
                     << "caller-visible object may contain dangling pointer state\n";
                break;
            }

            builder.message(body.str());
            result.diagnostics.push_back(builder.build());
        }
    }

} // namespace ctrace::stack::analyzer
