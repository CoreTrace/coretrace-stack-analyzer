#include "analysis/TOCTOUAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        struct PathEvent
        {
            const llvm::Instruction* inst = nullptr;
            const llvm::Value* root = nullptr;
            std::string literal;
            std::string api;
            unsigned order = 0;
        };

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

        static std::optional<unsigned> checkPathArgIndex(llvm::StringRef calleeName)
        {
            if (calleeName == "access" || calleeName == "stat" || calleeName == "lstat")
                return 0u;
            if (calleeName == "faccessat" || calleeName == "fstatat")
                return 1u;
            return std::nullopt;
        }

        static const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* value)
        {
            const llvm::Value* current = value->stripPointerCasts();
            for (unsigned depth = 0; depth < 4; ++depth)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(current);
                if (!load)
                    break;

                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    load->getPointerOperand()->stripPointerCasts());
                if (!slot || !slot->isStaticAlloca() || !slot->getAllocatedType()->isPointerTy())
                    break;

                const llvm::StoreInst* uniqueStore = nullptr;
                bool unsafe = false;
                for (const llvm::Use& use : slot->uses())
                {
                    const auto* user = use.getUser();
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafe = true;
                            break;
                        }
                        if (uniqueStore && uniqueStore != store)
                        {
                            unsafe = true;
                            break;
                        }
                        uniqueStore = store;
                        continue;
                    }

                    if (const auto* slotLoad = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (slotLoad->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafe = true;
                            break;
                        }
                        continue;
                    }

                    if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                    {
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic) ||
                            llvm::isa<llvm::LifetimeIntrinsic>(intrinsic))
                        {
                            continue;
                        }
                    }

                    unsafe = true;
                    break;
                }

                if (unsafe || !uniqueStore)
                    break;
                current = uniqueStore->getValueOperand()->stripPointerCasts();
            }

            return current;
        }

        static std::optional<unsigned> usePathArgIndex(llvm::StringRef calleeName)
        {
            if (calleeName == "open" || calleeName == "fopen")
                return 0u;
            if (calleeName == "openat")
                return 1u;
            return std::nullopt;
        }

        static std::optional<std::string> tryExtractStringLiteral(const llvm::Value* value)
        {
            if (!value)
                return std::nullopt;

            const llvm::Value* current = value->stripPointerCasts();
            for (unsigned depth = 0; depth < 8; ++depth)
            {
                if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                {
                    if (!global->hasInitializer())
                        return std::nullopt;

                    const llvm::Constant* init = global->getInitializer();
                    if (const auto* data = llvm::dyn_cast<llvm::ConstantDataSequential>(init))
                    {
                        if (data->isCString())
                            return data->getAsCString().str();
                    }
                    return std::nullopt;
                }

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

            return std::nullopt;
        }

        static bool likelySamePath(const PathEvent& lhs, const PathEvent& rhs)
        {
            if (lhs.root && rhs.root && lhs.root == rhs.root)
                return true;
            if (!lhs.literal.empty() && !rhs.literal.empty() && lhs.literal == rhs.literal)
                return true;
            return false;
        }
    } // namespace

    std::vector<TOCTOUIssue>
    analyzeTOCTOU(llvm::Module& mod,
                  const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<TOCTOUIssue> issues;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            std::vector<PathEvent> checks;
            std::vector<PathEvent> uses;
            unsigned order = 0;

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    ++order;

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call)
                        continue;

                    const llvm::Function* callee = getDirectCallee(*call);
                    if (!callee)
                        continue;

                    const llvm::StringRef canonicalName = canonicalCalleeName(callee->getName());
                    const std::optional<unsigned> checkArg = checkPathArgIndex(canonicalName);
                    const std::optional<unsigned> useArg = usePathArgIndex(canonicalName);
                    if (!checkArg && !useArg)
                        continue;

                    const unsigned argIndex = checkArg ? *checkArg : *useArg;
                    if (argIndex >= call->arg_size())
                        continue;

                    const llvm::Value* pathValue =
                        peelPointerFromSingleStoreSlot(call->getArgOperand(argIndex));
                    PathEvent event;
                    event.inst = &inst;
                    event.root = llvm::getUnderlyingObject(pathValue, 32);
                    event.literal = tryExtractStringLiteral(pathValue).value_or("");
                    event.api = canonicalName.str();
                    event.order = order;

                    if (checkArg)
                        checks.push_back(std::move(event));
                    else
                        uses.push_back(std::move(event));
                }
            }

            for (const PathEvent& useEvent : uses)
            {
                for (const PathEvent& checkEvent : checks)
                {
                    if (checkEvent.order >= useEvent.order)
                        continue;
                    if (!likelySamePath(checkEvent, useEvent))
                        continue;

                    TOCTOUIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.checkApi = checkEvent.api;
                    issue.useApi = useEvent.api;
                    issue.inst = useEvent.inst;
                    issues.push_back(std::move(issue));
                    break;
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
