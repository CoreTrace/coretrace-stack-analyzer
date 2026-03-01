#include "analysis/NullDerefAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"

#include <string>
#include <unordered_set>

#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool isLocalPointerSlot(const llvm::Value* pointer)
        {
            const auto* allocaInst = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                pointer ? pointer->stripPointerCasts() : nullptr);
            return allocaInst && allocaInst->isStaticAlloca();
        }

        static const llvm::Value* dereferencedPointer(const llvm::Instruction& inst)
        {
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst))
            {
                if (load->getType()->isPointerTy() && isLocalPointerSlot(load->getPointerOperand()))
                {
                    return nullptr;
                }
                return load->getPointerOperand();
            }
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
            {
                if (store->getValueOperand()->getType()->isPointerTy() &&
                    isLocalPointerSlot(store->getPointerOperand()))
                {
                    return nullptr;
                }
                return store->getPointerOperand();
            }
            if (const auto* atomic = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst))
                return atomic->getPointerOperand();
            if (const auto* cmp = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst))
                return cmp->getPointerOperand();
            return nullptr;
        }

        static std::string pointerDisplayName(const llvm::Value* pointer)
        {
            if (!pointer)
                return "<unknown>";

            const llvm::Value* root = llvm::getUnderlyingObject(pointer, 32);
            if (const auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(root))
            {
                if (allocaInst->hasName())
                    return allocaInst->getName().str();
            }
            if (const auto* arg = llvm::dyn_cast<llvm::Argument>(root))
            {
                if (arg->hasName())
                    return arg->getName().str();
                return "<arg>";
            }
            if (root && root->hasName())
                return root->getName().str();
            return "<pointer>";
        }

        static llvm::StringRef canonicalExternalCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("_"))
                name = name.drop_front();

            const std::size_t dollarPos = name.find('$');
            if (dollarPos != llvm::StringRef::npos)
                name = name.take_front(dollarPos);

            return name;
        }

        static bool isAllocatorLikeName(llvm::StringRef calleeName)
        {
            return calleeName == "malloc" || calleeName == "calloc" || calleeName == "realloc" ||
                   calleeName == "aligned_alloc";
        }

        static bool hasKnownNonNullReturn(const llvm::CallBase& call, const llvm::Function* callee)
        {
            if (call.hasRetAttr(llvm::Attribute::NonNull) ||
                call.hasRetAttr(llvm::Attribute::Dereferenceable))
            {
                return true;
            }
            if (!callee)
                return false;
            const llvm::AttributeList& attrs = callee->getAttributes();
            return attrs.hasRetAttr(llvm::Attribute::NonNull) ||
                   attrs.hasRetAttr(llvm::Attribute::Dereferenceable);
        }

        static bool isUncheckedAllocatorResult(const llvm::Value* value)
        {
            if (!value)
                return false;

            const auto* call = llvm::dyn_cast<llvm::CallBase>(value->stripPointerCasts());
            if (!call)
                return false;

            const llvm::Function* callee = call->getCalledFunction();
            if (!callee || !callee->isDeclaration())
                return false;

            const llvm::StringRef calleeName = canonicalExternalCalleeName(callee->getName());
            if (!isAllocatorLikeName(calleeName))
                return false;

            return !hasKnownNonNullReturn(*call, callee);
        }

        static const llvm::Value* stripPointerAddressOps(const llvm::Value* pointer)
        {
            if (!pointer)
                return nullptr;

            const llvm::Value* current = pointer;
            for (unsigned depth = 0; depth < 8; ++depth)
            {
                current = current->stripPointerCasts();
                const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current);
                if (!gep)
                    break;
                current = gep->getPointerOperand();
            }
            return current->stripPointerCasts();
        }

        static const llvm::Value* canonicalPointerIdentity(const llvm::Value* pointer)
        {
            const llvm::Value* current = stripPointerAddressOps(pointer);
            if (!current)
                return nullptr;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(current))
            {
                const llvm::Value* slot = load->getPointerOperand()->stripPointerCasts();
                if (llvm::isa<llvm::AllocaInst>(slot) || llvm::isa<llvm::Argument>(slot))
                    return slot;
            }

            if (const llvm::Value* underlying = llvm::getUnderlyingObject(current, 32))
                return underlying;

            return current;
        }

        static bool samePointerRoot(const llvm::Value* lhs, const llvm::Value* rhs)
        {
            if (!lhs || !rhs)
                return false;
            return canonicalPointerIdentity(lhs) == canonicalPointerIdentity(rhs);
        }

        static bool isNullValue(const llvm::Value* value)
        {
            return value && llvm::isa<llvm::ConstantPointerNull>(value->stripPointerCasts());
        }

        static bool conditionImpliesNullForSuccessor(const llvm::BranchInst& branch,
                                                     const llvm::BasicBlock& successor,
                                                     const llvm::Value*& outPointer)
        {
            if (!branch.isConditional())
                return false;

            const llvm::ICmpInst* cmp =
                llvm::dyn_cast<llvm::ICmpInst>(branch.getCondition()->stripPointerCasts());
            if (!cmp)
                return false;

            const llvm::Value* lhs = cmp->getOperand(0);
            const llvm::Value* rhs = cmp->getOperand(1);

            const llvm::Value* pointer = nullptr;
            bool nullOnTrue = false;

            if (isNullValue(lhs) && rhs->getType()->isPointerTy())
            {
                pointer = rhs;
            }
            else if (isNullValue(rhs) && lhs->getType()->isPointerTy())
            {
                pointer = lhs;
            }
            else
            {
                return false;
            }

            switch (cmp->getPredicate())
            {
            case llvm::ICmpInst::ICMP_EQ:
                nullOnTrue = true;
                break;
            case llvm::ICmpInst::ICMP_NE:
                nullOnTrue = false;
                break;
            default:
                return false;
            }

            const bool isTrueSucc = branch.getSuccessor(0) == &successor;
            const bool impliesNull = isTrueSucc ? nullOnTrue : !nullOnTrue;
            if (!impliesNull)
                return false;

            outPointer = pointer;
            return true;
        }

        static bool precedingStoreSetsNull(const llvm::Instruction& derefInst,
                                           const llvm::Value* pointerOperand)
        {
            const auto* pointerLoad =
                llvm::dyn_cast<llvm::LoadInst>(stripPointerAddressOps(pointerOperand));
            if (!pointerLoad)
                return false;

            const llvm::Value* slot = pointerLoad->getPointerOperand()->stripPointerCasts();
            if (!llvm::isa<llvm::AllocaInst>(slot))
                return false;

            const llvm::BasicBlock* block = derefInst.getParent();
            if (!block)
                return false;

            for (auto it = derefInst.getIterator(); it != block->begin();)
            {
                --it;
                const llvm::Instruction& candidate = *it;
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(&candidate);
                if (!store)
                    continue;
                if (store->getPointerOperand()->stripPointerCasts() != slot)
                    continue;
                return isNullValue(store->getValueOperand());
            }

            return false;
        }

        static const llvm::StoreInst*
        findNearestPrecedingStoreToSlot(const llvm::Instruction& derefInst, const llvm::Value* slot)
        {
            const llvm::BasicBlock* block = derefInst.getParent();
            if (!block || !slot)
                return nullptr;

            for (auto it = derefInst.getIterator(); it != block->begin();)
            {
                --it;
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(&*it);
                if (!store)
                    continue;
                if (store->getPointerOperand()->stripPointerCasts() == slot)
                    return store;
            }

            return nullptr;
        }

        static bool derefComesFromUncheckedAllocator(const llvm::Instruction& derefInst,
                                                     const llvm::Value* pointerOperand)
        {
            const llvm::Value* producer = stripPointerAddressOps(pointerOperand);
            if (!producer || !producer->getType()->isPointerTy())
                return false;

            if (isUncheckedAllocatorResult(producer))
                return true;

            const auto* pointerLoad = llvm::dyn_cast<llvm::LoadInst>(producer);
            if (!pointerLoad)
                return false;

            const llvm::Value* slot = pointerLoad->getPointerOperand()->stripPointerCasts();
            if (!llvm::isa<llvm::AllocaInst>(slot))
                return false;

            const llvm::StoreInst* originStore = findNearestPrecedingStoreToSlot(derefInst, slot);
            if (!originStore)
                return false;

            return isUncheckedAllocatorResult(originStore->getValueOperand());
        }
    } // namespace

    std::vector<NullDerefIssue>
    analyzeNullDereferences(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<NullDerefIssue> issues;
        std::unordered_set<const llvm::Instruction*> emitted;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    const llvm::Value* pointer = dereferencedPointer(inst);
                    if (!pointer || !pointer->getType()->isPointerTy())
                        continue;

                    NullDerefIssueKind kind = NullDerefIssueKind::DirectNullPointer;
                    bool shouldEmit = false;

                    if (isNullValue(pointer))
                    {
                        kind = NullDerefIssueKind::DirectNullPointer;
                        shouldEmit = true;
                    }
                    else if (precedingStoreSetsNull(inst, pointer))
                    {
                        kind = NullDerefIssueKind::NullStoredInLocalSlot;
                        shouldEmit = true;
                    }
                    else if (llvm::pred_size(&block) == 1)
                    {
                        const llvm::BasicBlock* pred = *llvm::pred_begin(&block);
                        const auto* branch =
                            pred ? llvm::dyn_cast<llvm::BranchInst>(pred->getTerminator())
                                 : nullptr;
                        const llvm::Value* nullComparedPointer = nullptr;
                        if (branch &&
                            conditionImpliesNullForSuccessor(*branch, block, nullComparedPointer) &&
                            samePointerRoot(pointer, nullComparedPointer))
                        {
                            kind = NullDerefIssueKind::NullBranchDereference;
                            shouldEmit = true;
                        }
                    }

                    if (!shouldEmit && derefComesFromUncheckedAllocator(inst, pointer))
                    {
                        kind = NullDerefIssueKind::UncheckedAllocatorResult;
                        shouldEmit = true;
                    }

                    if (!shouldEmit || !emitted.insert(&inst).second)
                        continue;

                    NullDerefIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.pointerName = pointerDisplayName(pointer);
                    issue.kind = kind;
                    issue.inst = &inst;
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
