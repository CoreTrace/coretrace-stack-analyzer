#include "analysis/IRValueUtils.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis
{
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

    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V, const llvm::Function& F)
    {
        using namespace llvm;

        // On enlève d'abord les cast (sext/zext/trunc, etc.) pour arriver
        // à la vraie valeur “de base”.
        const Value* cur = V;
        while (auto* cast = dyn_cast<const CastInst>(cur))
        {
            cur = cast->getOperand(0);
        }

        // Cas trivial : c'est déjà une constante entière.
        if (auto* C = dyn_cast<const ConstantInt>(cur))
            return C;

        // Cas -O0 typique : on compare un load d'une variable locale.
        auto* LI = dyn_cast<const LoadInst>(cur);
        if (!LI)
            return nullptr;

        const Value* ptr = LI->getPointerOperand();
        const ConstantInt* found = nullptr;

        // Version ultra-simple : on cherche un store de constante dans la fonction.
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
                    // On garde la dernière constante trouvée (si plusieurs stores, c'est naïf).
                    found = C;
                }
            }
        }
        return found;
    }
} // namespace ctrace::stack::analysis
