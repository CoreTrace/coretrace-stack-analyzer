// SPDX-License-Identifier: Apache-2.0
#include "StackPointerEscapeInternal.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <optional>

namespace ctrace::stack::analysis
{
    namespace
    {
        static void appendUniqueFunction(std::vector<const llvm::Function*>& out,
                                         const llvm::Function* F)
        {
            if (!F)
                return;
            if (std::find(out.begin(), out.end(), F) == out.end())
                out.push_back(F);
        }

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

        static void collectVTableEntries(const llvm::Constant* node,
                                         std::vector<const llvm::Constant*>& out)
        {
            if (!node)
                return;

            if (const auto* array = llvm::dyn_cast<llvm::ConstantArray>(node))
            {
                for (const llvm::Use& operand : array->operands())
                    collectVTableEntries(llvm::dyn_cast<llvm::Constant>(operand.get()), out);
                return;
            }
            if (const auto* strct = llvm::dyn_cast<llvm::ConstantStruct>(node))
            {
                for (const llvm::Use& operand : strct->operands())
                    collectVTableEntries(llvm::dyn_cast<llvm::Constant>(operand.get()), out);
                return;
            }
            if (node->getType()->isPointerTy())
                out.push_back(node);
        }

        static const llvm::Function* functionFromVTableEntry(const llvm::Constant* entry)
        {
            if (!entry)
                return nullptr;
            return llvm::dyn_cast<llvm::Function>(entry->stripPointerCasts());
        }

        static std::optional<unsigned> extractVirtualDispatchSlotIndex(const llvm::CallBase& CB)
        {
            const llvm::Value* calledVal = CB.getCalledOperand();
            const auto* fnLoad =
                calledVal ? llvm::dyn_cast<llvm::LoadInst>(calledVal->stripPointerCasts())
                          : nullptr;
            if (!fnLoad)
                return std::nullopt;

            const auto* slotGEP = llvm::dyn_cast<llvm::GetElementPtrInst>(
                fnLoad->getPointerOperand()->stripPointerCasts());
            if (!slotGEP || slotGEP->getNumOperands() < 2)
                return std::nullopt;

            const auto* vtableLoad =
                llvm::dyn_cast<llvm::LoadInst>(slotGEP->getPointerOperand()->stripPointerCasts());
            if (!vtableLoad)
                return std::nullopt;
            if (!isLikelyVPtrLoadFromObjectRoot(*vtableLoad))
                return std::nullopt;

            const auto* slotIndex = llvm::dyn_cast<llvm::ConstantInt>(
                slotGEP->getOperand(slotGEP->getNumOperands() - 1));
            if (!slotIndex || slotIndex->isNegative())
                return std::nullopt;

            return static_cast<unsigned>(slotIndex->getZExtValue());
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

        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (!global.hasInitializer())
                continue;

            llvm::StringRef name = global.getName();
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (!name.starts_with("_ZTV"))
                continue;

            std::vector<const llvm::Constant*> entries;
            collectVTableEntries(global.getInitializer(), entries);
            if (entries.size() <= 2)
                continue;

            for (std::size_t i = 2; i < entries.size(); ++i)
            {
                const llvm::Function* target = functionFromVTableEntry(entries[i]);
                if (!target || target->isDeclaration())
                    continue;

                const unsigned slotIndex = static_cast<unsigned>(i - 2);
                std::vector<const llvm::Function*>& perSlot =
                    virtualCandidatesByFunctionTypeAndSlot[target->getFunctionType()][slotIndex];
                appendUniqueFunction(perSlot, target);
            }
        }
    }

    const std::vector<const llvm::Function*>&
    IndirectTargetResolver::candidatesForCall(const llvm::CallBase& CB) const
    {
        const llvm::FunctionType* calleeTy = CB.getFunctionType();
        if (!calleeTy)
            return empty;

        if (const std::optional<unsigned> slotIndex = extractVirtualDispatchSlotIndex(CB))
        {
            auto byType = virtualCandidatesByFunctionTypeAndSlot.find(calleeTy);
            if (byType != virtualCandidatesByFunctionTypeAndSlot.end())
            {
                auto bySlot = byType->second.find(*slotIndex);
                if (bySlot != byType->second.end() && !bySlot->second.empty())
                    return bySlot->second;
            }
        }

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
