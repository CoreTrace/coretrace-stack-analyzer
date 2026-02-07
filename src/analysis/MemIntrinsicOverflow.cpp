#include "analysis/MemIntrinsicOverflow.hpp"

#include <optional>

#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static std::optional<StackSize> getAllocaTotalSizeBytes(const llvm::AllocaInst* AI,
                                                                const llvm::DataLayout& DL)
        {
            using namespace llvm;

            Type* allocatedTy = AI->getAllocatedType();

            if (!AI->isArrayAllocation())
            {
                return DL.getTypeAllocSize(allocatedTy);
            }

            if (auto* C = dyn_cast<ConstantInt>(AI->getArraySize()))
            {
                uint64_t count = C->getZExtValue();
                uint64_t elemSize = DL.getTypeAllocSize(allocatedTy);
                return count * elemSize;
            }

            return std::nullopt;
        }

        static void analyzeMemIntrinsicOverflowsInFunction(llvm::Function& F,
                                                           const llvm::DataLayout& DL,
                                                           std::vector<MemIntrinsicIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* CB = dyn_cast<CallBase>(&I);
                    if (!CB)
                        continue;

                    Function* callee = CB->getCalledFunction();
                    if (!callee)
                        continue;

                    StringRef name = callee->getName();

                    enum class MemKind
                    {
                        None,
                        MemCpy,
                        MemSet,
                        MemMove
                    };
                    MemKind kind = MemKind::None;

                    if (auto* II = dyn_cast<IntrinsicInst>(CB))
                    {
                        switch (II->getIntrinsicID())
                        {
                        case Intrinsic::memcpy:
                            kind = MemKind::MemCpy;
                            break;
                        case Intrinsic::memset:
                            kind = MemKind::MemSet;
                            break;
                        case Intrinsic::memmove:
                            kind = MemKind::MemMove;
                            break;
                        default:
                            break;
                        }
                    }

                    if (kind == MemKind::None)
                    {
                        if (name == "memcpy" || name.contains("memcpy"))
                            kind = MemKind::MemCpy;
                        else if (name == "memset" || name.contains("memset"))
                            kind = MemKind::MemSet;
                        else if (name == "memmove" || name.contains("memmove"))
                            kind = MemKind::MemMove;
                    }

                    if (kind == MemKind::None)
                        continue;

                    if (CB->arg_size() < 3)
                        continue;

                    Value* dest = CB->getArgOperand(0);

                    const Value* cur = dest->stripPointerCasts();
                    if (auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                    {
                        cur = GEP->getPointerOperand();
                    }
                    const AllocaInst* AI = dyn_cast<AllocaInst>(cur);
                    if (!AI)
                        continue;

                    auto maybeSize = getAllocaTotalSizeBytes(AI, DL);
                    if (!maybeSize)
                        continue;
                    StackSize destBytes = *maybeSize;

                    Value* lenV = CB->getArgOperand(2);
                    auto* lenC = dyn_cast<ConstantInt>(lenV);
                    if (!lenC)
                        continue;

                    uint64_t len = lenC->getZExtValue();
                    if (len <= destBytes)
                        continue;

                    MemIntrinsicIssue issue;
                    issue.funcName = F.getName().str();
                    issue.varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                    issue.destSizeBytes = destBytes;
                    issue.lengthBytes = len;
                    issue.inst = &I;

                    switch (kind)
                    {
                    case MemKind::MemCpy:
                        issue.intrinsicName = "memcpy";
                        break;
                    case MemKind::MemSet:
                        issue.intrinsicName = "memset";
                        break;
                    case MemKind::MemMove:
                        issue.intrinsicName = "memmove";
                        break;
                    default:
                        break;
                    }

                    out.push_back(std::move(issue));
                }
            }
        }
    } // namespace

    std::vector<MemIntrinsicIssue>
    analyzeMemIntrinsicOverflows(llvm::Module& mod,
                                 const llvm::DataLayout& DL,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<MemIntrinsicIssue> issues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeMemIntrinsicOverflowsInFunction(F, DL, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
