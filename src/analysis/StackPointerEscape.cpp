#include "analysis/StackPointerEscape.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool isStdLibCalleeName(llvm::StringRef name)
        {
            return name.starts_with("_ZNSt3__1") || name.starts_with("_ZSt") ||
                   name.starts_with("_ZNSt") || name.starts_with("__cxx");
        }

        static bool isStdLibCallee(const llvm::Function* F)
        {
            if (!F)
                return false;
            return isStdLibCalleeName(F->getName());
        }

        static void analyzeStackPointerEscapesInFunction(llvm::Function& F,
                                                         std::vector<StackPointerEscapeIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;

                    SmallPtrSet<const Value*, 16> visited;
                    SmallVector<const Value*, 8> worklist;
                    worklist.push_back(AI);

                    while (!worklist.empty())
                    {
                        const Value* V = worklist.back();
                        worklist.pop_back();
                        if (visited.contains(V))
                            continue;
                        visited.insert(V);

                        for (const Use& U : V->uses())
                        {
                            const User* Usr = U.getUser();

                            if (auto* RI = dyn_cast<ReturnInst>(Usr))
                            {
                                StackPointerEscapeIssue issue;
                                issue.funcName = F.getName().str();
                                issue.varName =
                                    AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                                issue.escapeKind = "return";
                                issue.targetName = {};
                                issue.inst = RI;
                                out.push_back(std::move(issue));
                                continue;
                            }

                            if (auto* SI = dyn_cast<StoreInst>(Usr))
                            {
                                if (SI->getValueOperand() == V)
                                {
                                    const Value* dstRaw = SI->getPointerOperand();
                                    const Value* dst = dstRaw->stripPointerCasts();

                                    if (auto* GV = dyn_cast<GlobalVariable>(dst))
                                    {
                                        StackPointerEscapeIssue issue;
                                        issue.funcName = F.getName().str();
                                        issue.varName = AI->hasName() ? AI->getName().str()
                                                                      : std::string("<unnamed>");
                                        issue.escapeKind = "store_global";
                                        issue.targetName =
                                            GV->hasName() ? GV->getName().str() : std::string{};
                                        issue.inst = SI;
                                        out.push_back(std::move(issue));
                                        continue;
                                    }

                                    if (!isa<AllocaInst>(dst))
                                    {
                                        StackPointerEscapeIssue issue;
                                        issue.funcName = F.getName().str();
                                        issue.varName = AI->hasName() ? AI->getName().str()
                                                                      : std::string("<unnamed>");
                                        issue.escapeKind = "store_unknown";
                                        issue.targetName =
                                            dst->hasName() ? dst->getName().str() : std::string{};
                                        issue.inst = SI;
                                        out.push_back(std::move(issue));
                                        continue;
                                    }

                                    const AllocaInst* dstAI = cast<AllocaInst>(dst);
                                    worklist.push_back(dstAI);
                                }
                                continue;
                            }

                            if (auto* CB = dyn_cast<CallBase>(Usr))
                            {
                                for (unsigned argIndex = 0; argIndex < CB->arg_size(); ++argIndex)
                                {
                                    if (CB->getArgOperand(argIndex) != V)
                                        continue;

                                    const Value* calledVal = CB->getCalledOperand();
                                    const Value* calledStripped =
                                        calledVal ? calledVal->stripPointerCasts() : nullptr;
                                    const Function* directCallee =
                                        calledStripped ? dyn_cast<Function>(calledStripped)
                                                       : nullptr;
                                    if (CB->paramHasAttr(argIndex, llvm::Attribute::NoCapture) ||
                                        CB->paramHasAttr(argIndex, llvm::Attribute::ByVal) ||
                                        CB->paramHasAttr(argIndex, llvm::Attribute::ByRef))
                                    {
                                        continue;
                                    }
                                    if (directCallee)
                                    {
                                        llvm::StringRef calleeName = directCallee->getName();
                                        if (calleeName.contains("unique_ptr") ||
                                            calleeName.contains("make_unique"))
                                        {
                                            continue;
                                        }
                                        if (isStdLibCallee(directCallee))
                                        {
                                            continue;
                                        }
                                    }

                                    StackPointerEscapeIssue issue;
                                    issue.funcName = F.getName().str();
                                    issue.varName = AI->hasName() ? AI->getName().str()
                                                                  : std::string("<unnamed>");
                                    issue.inst = cast<Instruction>(CB);

                                    if (!directCallee)
                                    {
                                        issue.escapeKind = "call_callback";
                                        issue.targetName.clear();
                                        out.push_back(std::move(issue));
                                    }
                                    else
                                    {
#ifdef CT_DISABLE_CALL_ARG
                                        issue.escapeKind = "call_arg";
                                        issue.targetName = directCallee->hasName()
                                                               ? directCallee->getName().str()
                                                               : std::string{};
                                        out.push_back(std::move(issue));
#endif
                                    }
                                }

                                continue;
                            }

                            if (auto* BC = dyn_cast<BitCastInst>(Usr))
                            {
                                if (BC->getType()->isPointerTy())
                                    worklist.push_back(BC);
                                continue;
                            }
                            if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                            {
                                worklist.push_back(GEP);
                                continue;
                            }
                            if (auto* PN = dyn_cast<PHINode>(Usr))
                            {
                                if (PN->getType()->isPointerTy())
                                    worklist.push_back(PN);
                                continue;
                            }
                            if (auto* Sel = dyn_cast<SelectInst>(Usr))
                            {
                                if (Sel->getType()->isPointerTy())
                                    worklist.push_back(Sel);
                                continue;
                            }
                        }
                    }
                }
            }
        }
    } // namespace

    std::vector<StackPointerEscapeIssue>
    analyzeStackPointerEscapes(llvm::Module& mod,
                               const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<StackPointerEscapeIssue> issues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackPointerEscapesInFunction(F, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
