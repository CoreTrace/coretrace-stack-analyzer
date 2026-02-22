#include "StackPointerEscapeInternal.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool isLikelyItaniumCppMethodName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            return name.starts_with("_ZN");
        }

        static bool isLikelyVPtrLoadFromObjectRoot(const llvm::LoadInst& tablePtrLoad)
        {
            // Typical C++ virtual dispatch reads vptr from the root object pointer.
            // Reject obvious field-offset loads (common in C callback tables like obj->ops->fn).
            const llvm::Value* ptrOp = tablePtrLoad.getPointerOperand()->stripPointerCasts();
            const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(ptrOp);
            if (!gep)
                return true;
            for (unsigned i = 1; i < gep->getNumOperands(); ++i)
            {
                const auto* idx = llvm::dyn_cast<llvm::ConstantInt>(gep->getOperand(i));
                if (!idx || !idx->isZero())
                    return false;
            }
            return true;
        }
    } // namespace

    IndirectTargetResolver::IndirectTargetResolver(const llvm::Module& module)
    {
        for (const llvm::Function& candidate : module)
        {
            if (candidate.isDeclaration())
                continue;
            if (!candidate.hasAddressTaken())
                continue;

            candidatesByFunctionType[candidate.getFunctionType()].push_back(&candidate);
        }
    }

    const std::vector<const llvm::Function*>&
    IndirectTargetResolver::candidatesForCall(const llvm::CallBase& CB) const
    {
        const llvm::FunctionType* calleeTy = CB.getFunctionType();
        if (!calleeTy)
            return empty;
        auto it = candidatesByFunctionType.find(calleeTy);
        if (it == candidatesByFunctionType.end())
            return empty;
        return it->second;
    }

    bool isLikelyVirtualDispatchCall(const llvm::CallBase& CB)
    {
        const llvm::Function* caller = CB.getFunction();
        if (!caller || !isLikelyItaniumCppMethodName(caller->getName()))
            return false;

        const llvm::Value* calledVal = CB.getCalledOperand();
        const auto* fnLoad =
            calledVal ? llvm::dyn_cast<llvm::LoadInst>(calledVal->stripPointerCasts()) : nullptr;
        if (!fnLoad)
            return false;

        const auto* slotGEP = llvm::dyn_cast<llvm::GetElementPtrInst>(
            fnLoad->getPointerOperand()->stripPointerCasts());
        if (!slotGEP)
            return false;

        const auto* vtableLoad =
            llvm::dyn_cast<llvm::LoadInst>(slotGEP->getPointerOperand()->stripPointerCasts());
        if (!vtableLoad)
            return false;

        if (!isLikelyVPtrLoadFromObjectRoot(*vtableLoad))
            return false;

        return true;
    }
} // namespace ctrace::stack::analysis
