// SPDX-License-Identifier: Apache-2.0
#include "analysis/IRValueUtils.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis
{
    bool isLikelyCompilerTemporaryName(llvm::StringRef name)
    {
        if (name.empty() || name == "<unnamed>")
            return true;

        if (name.starts_with("ref.tmp") || name.starts_with("agg.tmp") ||
            name.starts_with("coerce") || name.starts_with("__range") ||
            name.starts_with("__begin") || name.starts_with("__end") ||
            name.starts_with("retval") || name.starts_with("exn.slot") ||
            name.starts_with("ehselector.slot"))
        {
            return true;
        }

        if (name.ends_with(".addr"))
            return true;

        return name.contains(".tmp");
    }

    std::string deriveAllocaName(const llvm::AllocaInst* AI)
    {
        using namespace llvm;

        if (!AI)
            return std::string("<unnamed>");
        if (AI->hasName())
            return AI->getName().str();

        SmallPtrSet<const Value*, 16> visited;
        SmallVector<const Value*, 8> worklist;
        worklist.push_back(AI);

        while (!worklist.empty())
        {
            const Value* V = worklist.back();
            worklist.pop_back();
            if (!visited.insert(V).second)
                continue;

            for (const Use& U : V->uses())
            {
                const User* Usr = U.getUser();

                if (auto* DVI = dyn_cast<DbgValueInst>(Usr))
                {
                    if (auto* var = DVI->getVariable())
                    {
                        if (!var->getName().empty())
                            return var->getName().str();
                    }
                    continue;
                }

                if (auto* SI = dyn_cast<StoreInst>(Usr))
                {
                    if (SI->getValueOperand() != V)
                        continue;
                    const Value* dst = SI->getPointerOperand()->stripPointerCasts();
                    if (auto* dstAI = dyn_cast<AllocaInst>(dst))
                    {
                        if (dstAI->hasName())
                            return dstAI->getName().str();
                    }
                    worklist.push_back(dst);
                    continue;
                }

                if (auto* BC = dyn_cast<BitCastInst>(Usr))
                {
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

        return std::string("<unnamed>");
    }

    AllocaOrigin classifyAllocaOrigin(const llvm::AllocaInst* AI)
    {
        using namespace llvm;

        if (!AI)
            return AllocaOrigin::Unknown;

        bool sawUserDebugVar = false;
        bool sawArtificialDebugVar = false;
        for (DbgDeclareInst* declareInst : findDbgDeclares(const_cast<AllocaInst*>(AI)))
        {
            if (!declareInst)
                continue;
            const DILocalVariable* var = declareInst->getVariable();
            if (!var)
                continue;
            if (var->isArtificial())
                sawArtificialDebugVar = true;
            else
                sawUserDebugVar = true;
        }

        if (sawUserDebugVar && !sawArtificialDebugVar)
            return AllocaOrigin::User;
        if (sawArtificialDebugVar && !sawUserDebugVar)
            return AllocaOrigin::CompilerGenerated;

        // Conservative fallback when variable debug metadata is absent:
        // treat only explicit compiler-like names as compiler-generated.
        const std::string derivedName = deriveAllocaName(AI);
        if (!derivedName.empty() && derivedName != "<unnamed>" &&
            isLikelyCompilerTemporaryName(derivedName))
        {
            return AllocaOrigin::CompilerGenerated;
        }

        if (AI->hasName())
        {
            llvm::StringRef irName = AI->getName();
            if (!irName.empty() && irName != "<unnamed>" && isLikelyCompilerTemporaryName(irName))
            {
                return AllocaOrigin::CompilerGenerated;
            }
        }

        return AllocaOrigin::Unknown;
    }

    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V, const llvm::Function& F)
    {
        using namespace llvm;

        // First remove casts (sext/zext/trunc, etc.) to reach the real base value.
        const Value* cur = V;
        while (auto* cast = dyn_cast<const CastInst>(cur))
        {
            cur = cast->getOperand(0);
        }

        // Trivial case: already an integer constant.
        if (auto* C = dyn_cast<const ConstantInt>(cur))
            return C;

        // Typical -O0 case: comparing a load from a local variable.
        auto* LI = dyn_cast<const LoadInst>(cur);
        if (!LI)
            return nullptr;

        const Value* ptr = LI->getPointerOperand();
        const ConstantInt* found = nullptr;

        // Ultra-simple version: look for a constant store in the function.
        for (const BasicBlock& BB : F)
        {
            for (const Instruction& I : BB)
            {
                auto* SI = dyn_cast<const StoreInst>(&I);
                if (!SI)
                    continue;
                if (SI->getPointerOperand() != ptr)
                    continue;
                if (auto* C = dyn_cast<const ConstantInt>(SI->getValueOperand()))
                {
                    // Keep the last constant found (if multiple stores, this is naive).
                    found = C;
                }
            }
        }
        return found;
    }
} // namespace ctrace::stack::analysis
