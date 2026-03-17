#include "analysis/CommandInjectionAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"

#include <optional>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static const llvm::Function* getDirectCallee(const llvm::CallBase& call)
        {
            if (const llvm::Function* direct = call.getCalledFunction())
                return direct;
            const llvm::Value* called = call.getCalledOperand();
            if (!called)
                return nullptr;
            return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
        }

        static llvm::StringRef canonicalCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("_"))
                name = name.drop_front();
            return name;
        }

        static std::optional<unsigned> shellCommandArgIndex(llvm::StringRef calleeName)
        {
            // Shell-based sinks: command parsing happens inside a shell interpreter.
            if (calleeName == "system" || calleeName == "popen")
                return 0u;
            return std::nullopt;
        }

        static bool isStringConstantGlobal(const llvm::GlobalVariable& global)
        {
            if (!global.hasInitializer())
                return false;

            const llvm::Constant* init = global.getInitializer();
            if (const auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(init))
                return data->isCString();
            return false;
        }

        static bool isCompileTimeConstantString(const llvm::Value* value)
        {
            if (!value)
                return false;

            const llvm::Value* current = value->stripPointerCasts();
            for (unsigned depth = 0; depth < 8; ++depth)
            {
                if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                    return isStringConstantGlobal(*global);

                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    current = gep->getPointerOperand()->stripPointerCasts();
                    continue;
                }

                if (const auto* expr = llvm::dyn_cast<llvm::ConstantExpr>(current))
                {
                    if (expr->isCast() || expr->getOpcode() == llvm::Instruction::GetElementPtr)
                    {
                        current = expr->getOperand(0)->stripPointerCasts();
                        continue;
                    }
                }

                break;
            }

            return false;
        }
    } // namespace

    std::vector<CommandInjectionIssue>
    analyzeCommandInjection(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<CommandInjectionIssue> issues;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call)
                        continue;

                    const llvm::Function* callee = getDirectCallee(*call);
                    if (!callee)
                        continue;

                    const llvm::StringRef canonicalName = canonicalCalleeName(callee->getName());
                    const std::optional<unsigned> commandArg = shellCommandArgIndex(canonicalName);
                    if (!commandArg || *commandArg >= call->arg_size())
                        continue;

                    const llvm::Value* commandValue = call->getArgOperand(*commandArg);
                    if (isCompileTimeConstantString(commandValue))
                        continue;

                    CommandInjectionIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.sinkName = canonicalName.str();
                    issue.inst = &inst;
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }

    namespace
    {
        template <typename CallBaseT>
        static void collectCommandInjectionFromCallBases(
            const llvm::Function& function,
            const std::vector<const CallBaseT*>& callBases,
            std::vector<CommandInjectionIssue>& issues)
        {
            for (const CallBaseT* call : callBases)
            {
                const llvm::Function* callee = getDirectCallee(*call);
                if (!callee)
                    continue;

                const llvm::StringRef canonicalName = canonicalCalleeName(callee->getName());
                const std::optional<unsigned> commandArg = shellCommandArgIndex(canonicalName);
                if (!commandArg || *commandArg >= call->arg_size())
                    continue;

                const llvm::Value* commandValue = call->getArgOperand(*commandArg);
                if (isCompileTimeConstantString(commandValue))
                    continue;

                CommandInjectionIssue issue;
                issue.funcName = function.getName().str();
                issue.filePath = getFunctionSourcePath(function);
                issue.sinkName = canonicalName.str();
                issue.inst = call;
                issues.push_back(std::move(issue));
            }
        }
    } // namespace

    std::vector<CommandInjectionIssue>
    analyzeCommandInjectionCached(const llvm::Function& function,
                                  const std::vector<const llvm::CallInst*>& calls,
                                  const std::vector<const llvm::InvokeInst*>& invokes)
    {
        std::vector<CommandInjectionIssue> issues;
        collectCommandInjectionFromCallBases(function, calls, issues);
        collectCommandInjectionFromCallBases(function, invokes, issues);
        return issues;
    }
} // namespace ctrace::stack::analysis
