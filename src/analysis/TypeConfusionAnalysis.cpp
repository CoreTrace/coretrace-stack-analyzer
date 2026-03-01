#include "analysis/TypeConfusionAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"

#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        struct ViewObservation
        {
            const llvm::StructType* viewType = nullptr;
            std::uint64_t viewSizeBytes = 0;
            std::uint64_t accessOffsetBytes = 0;
            const llvm::Instruction* inst = nullptr;
            llvm::SmallVector<const llvm::StructType*, 6> structChain;
        };

        static std::string structDisplayName(const llvm::StructType* type)
        {
            if (!type)
                return "<struct>";
            if (type->hasName())
                return type->getName().str();
            return "<anonymous-struct>";
        }

        static void collectStructTypeChain(const llvm::Value* pointer,
                                           llvm::SmallVector<const llvm::StructType*, 6>& out)
        {
            if (!pointer)
                return;

            const llvm::Value* current = pointer;
            std::unordered_set<const llvm::StructType*> seen(out.begin(), out.end());
            for (unsigned depth = 0; depth < 20; ++depth)
            {
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    if (const auto* structType =
                            llvm::dyn_cast<llvm::StructType>(gep->getSourceElementType()))
                    {
                        if (!structType->isOpaque() && seen.insert(structType).second)
                            out.push_back(structType);
                    }
                    current = gep->getPointerOperand();
                    continue;
                }

                if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(current))
                {
                    current = cast->getOperand(0);
                    continue;
                }

                if (const auto* expr = llvm::dyn_cast<llvm::ConstantExpr>(current))
                {
                    if (expr->isCast() || expr->getOpcode() == llvm::Instruction::GetElementPtr)
                    {
                        current = expr->getOperand(0);
                        continue;
                    }
                }

                break;
            }
        }

        static const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* value)
        {
            const llvm::Value* current = value ? value->stripPointerCasts() : nullptr;
            for (unsigned depth = 0; current && depth < 6; ++depth)
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

        static bool containsStructTypeRecursive(const llvm::Type* haystack,
                                                const llvm::StructType* needle,
                                                std::unordered_set<const llvm::Type*>& visiting,
                                                unsigned depth = 0)
        {
            if (!haystack || !needle || depth > 24)
                return false;
            if (haystack == needle)
                return true;
            if (!visiting.insert(haystack).second)
                return false;

            if (const auto* structType = llvm::dyn_cast<llvm::StructType>(haystack))
            {
                if (structType->isOpaque())
                    return false;
                for (llvm::Type* elemType : structType->elements())
                {
                    if (containsStructTypeRecursive(elemType, needle, visiting, depth + 1))
                        return true;
                }
                return false;
            }

            if (const auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(haystack))
                return containsStructTypeRecursive(arrayType->getElementType(), needle, visiting,
                                                   depth + 1);

            if (const auto* vectorType = llvm::dyn_cast<llvm::VectorType>(haystack))
                return containsStructTypeRecursive(vectorType->getElementType(), needle, visiting,
                                                   depth + 1);

            return false;
        }

        static bool structContainsType(const llvm::StructType* container,
                                       const llvm::StructType* nested)
        {
            if (!container || !nested || container->isOpaque())
                return false;
            std::unordered_set<const llvm::Type*> visiting;
            return containsStructTypeRecursive(container, nested, visiting, 0);
        }

        static const llvm::StructType* getConcreteRootStructType(const llvm::Value* root)
        {
            const llvm::Value* stripped = root ? root->stripPointerCasts() : nullptr;
            if (!stripped)
                return nullptr;

            const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(stripped);
            if (!alloca)
                return nullptr;

            const auto* structType = llvm::dyn_cast<llvm::StructType>(alloca->getAllocatedType());
            if (!structType || structType->isOpaque())
                return nullptr;
            return structType;
        }

        static bool hasCommonTypeContext(const std::unordered_set<const llvm::StructType*>& lhs,
                                         const std::unordered_set<const llvm::StructType*>& rhs)
        {
            if (lhs.empty() || rhs.empty())
                return false;
            const auto& smaller = lhs.size() <= rhs.size() ? lhs : rhs;
            const auto& larger = lhs.size() <= rhs.size() ? rhs : lhs;
            for (const llvm::StructType* type : smaller)
            {
                if (larger.find(type) != larger.end())
                    return true;
            }
            return false;
        }

        static bool
        contextContainsSubobjectView(const std::unordered_set<const llvm::StructType*>& context,
                                     const llvm::StructType* view)
        {
            if (!view)
                return false;
            for (const llvm::StructType* contextType : context)
            {
                if (!contextType)
                    continue;
                if (contextType == view || structContainsType(contextType, view))
                    return true;
            }
            return false;
        }

        static bool viewsLikelyCompatible(
            const llvm::StructType* lhs, const llvm::StructType* rhs,
            const std::unordered_map<const llvm::StructType*,
                                     std::unordered_set<const llvm::StructType*>>& contextsByView,
            const llvm::StructType* concreteRootType)
        {
            if (!lhs || !rhs)
                return false;
            if (lhs == rhs)
                return true;

            // Typical "field-of-struct" patterns: one view is a nested subobject of the other.
            if (structContainsType(lhs, rhs) || structContainsType(rhs, lhs))
                return true;

            // If both views are observed under a shared structural ancestor in IR GEP chains,
            // they are likely sibling subobjects of the same aggregate rather than type confusion.
            const auto lhsIt = contextsByView.find(lhs);
            const auto rhsIt = contextsByView.find(rhs);
            if (lhsIt != contextsByView.end() && rhsIt != contextsByView.end() &&
                hasCommonTypeContext(lhsIt->second, rhsIt->second))
            {
                return true;
            }

            // If one view's structural context contains the other view type as a subobject,
            // this is typically a legitimate "same object, different subobject path" pattern
            // (derived/base plus member aggregates, etc.).
            if (lhsIt != contextsByView.end() && contextContainsSubobjectView(lhsIt->second, rhs))
            {
                return true;
            }
            if (rhsIt != contextsByView.end() && contextContainsSubobjectView(rhsIt->second, lhs))
            {
                return true;
            }

            // When the root object type is known (local alloca), keep only mismatches that cannot
            // be explained as legal subobjects inside that root aggregate.
            if (concreteRootType && structContainsType(concreteRootType, lhs) &&
                structContainsType(concreteRootType, rhs))
            {
                return true;
            }

            return false;
        }

        static void
        collectObservation(const llvm::Value* pointer, const llvm::Instruction& atInst,
                           const llvm::DataLayout& dataLayout,
                           std::map<const llvm::Value*, std::vector<ViewObservation>>& outByRoot)
        {
            if (!pointer || !pointer->getType()->isPointerTy())
                return;

            llvm::SmallVector<const llvm::StructType*, 6> structChain;
            collectStructTypeChain(pointer, structChain);
            const llvm::Value* peeledPointer = peelPointerFromSingleStoreSlot(pointer);
            if (peeledPointer && peeledPointer != pointer)
                collectStructTypeChain(peeledPointer, structChain);
            if (structChain.empty())
                return;
            const llvm::StructType* viewType = structChain.front();

            int64_t offset = 0;
            const llvm::Value* base =
                llvm::GetPointerBaseWithConstantOffset(pointer, offset, dataLayout, true);
            if (!base || offset < 0)
                return;

            const llvm::Value* root = peelPointerFromSingleStoreSlot(base);
            root = llvm::getUnderlyingObject(root, 32);
            root = peelPointerFromSingleStoreSlot(root);
            if (!root)
                return;

            const std::uint64_t viewSize =
                dataLayout.getTypeAllocSize(const_cast<llvm::StructType*>(viewType));

            ViewObservation obs;
            obs.viewType = viewType;
            obs.viewSizeBytes = viewSize;
            obs.accessOffsetBytes = static_cast<std::uint64_t>(offset);
            obs.inst = &atInst;
            obs.structChain = structChain;

            outByRoot[root].push_back(std::move(obs));
        }
    } // namespace

    std::vector<TypeConfusionIssue>
    analyzeTypeConfusions(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<TypeConfusionIssue> issues;
        std::unordered_set<const llvm::Instruction*> emitted;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            std::map<const llvm::Value*, std::vector<ViewObservation>> observationsByRoot;

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst))
                    {
                        collectObservation(load->getPointerOperand(), inst, dataLayout,
                                           observationsByRoot);
                        continue;
                    }

                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                    {
                        collectObservation(store->getPointerOperand(), inst, dataLayout,
                                           observationsByRoot);
                        continue;
                    }

                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
                    {
                        for (const llvm::Value* argument : call->args())
                            collectObservation(argument, inst, dataLayout, observationsByRoot);
                    }
                }
            }

            for (const auto& [root, observations] : observationsByRoot)
            {
                if (observations.size() < 2)
                    continue;

                std::uint64_t smallestViewSize = std::numeric_limits<std::uint64_t>::max();
                const llvm::StructType* smallestViewType = nullptr;
                std::unordered_set<const llvm::StructType*> distinctViews;
                std::unordered_map<const llvm::StructType*,
                                   std::unordered_set<const llvm::StructType*>>
                    contextsByView;
                distinctViews.reserve(observations.size());

                for (const ViewObservation& obs : observations)
                {
                    if (!obs.viewType)
                        continue;
                    distinctViews.insert(obs.viewType);
                    auto& context = contextsByView[obs.viewType];
                    context.insert(obs.viewType);
                    for (const llvm::StructType* type : obs.structChain)
                        context.insert(type);
                    if (obs.viewSizeBytes < smallestViewSize)
                    {
                        smallestViewSize = obs.viewSizeBytes;
                        smallestViewType = obs.viewType;
                    }
                }

                if (!smallestViewType || distinctViews.size() < 2)
                    continue;

                const llvm::StructType* concreteRootType = getConcreteRootStructType(root);
                for (const ViewObservation& obs : observations)
                {
                    if (!obs.inst || obs.viewType == smallestViewType)
                        continue;
                    if (obs.accessOffsetBytes < smallestViewSize)
                        continue;
                    if (viewsLikelyCompatible(smallestViewType, obs.viewType, contextsByView,
                                              concreteRootType))
                    {
                        continue;
                    }
                    if (!emitted.insert(obs.inst).second)
                        continue;

                    TypeConfusionIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.smallerViewType = structDisplayName(smallestViewType);
                    issue.accessedViewType = structDisplayName(obs.viewType);
                    issue.smallerViewSizeBytes = smallestViewSize;
                    issue.accessOffsetBytes = obs.accessOffsetBytes;
                    issue.inst = obs.inst;
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
