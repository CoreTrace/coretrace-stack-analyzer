#include "analysis/MemIntrinsicOverflow.hpp"
#include "analysis/BufferWriteModel.hpp"

#include <iostream>
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
        struct ResolvedSink
        {
            std::string displayName;
            unsigned destArgIndex = 0;
            unsigned sizeArgIndex = 0;
            std::uint64_t valid : 1 = false;
            std::uint64_t hasExplicitLength : 1 = false;
            std::uint64_t reservedFlags : 62 = 0;
        };

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

        static llvm::Function* resolveDirectCallee(llvm::CallBase* CB)
        {
            using namespace llvm;
            if (!CB)
                return nullptr;
            if (Function* direct = CB->getCalledFunction())
                return direct;
            Value* callee = CB->getCalledOperand();
            if (!callee)
                return nullptr;
            return dyn_cast<Function>(callee->stripPointerCasts());
        }

        static ResolvedSink resolveBuiltInSink(llvm::CallBase* CB)
        {
            using namespace llvm;
            ResolvedSink sink;
            if (!CB)
                return sink;

            if (auto* II = dyn_cast<IntrinsicInst>(CB))
            {
                switch (II->getIntrinsicID())
                {
                case Intrinsic::memcpy:
                    sink.valid = true;
                    sink.hasExplicitLength = true;
                    sink.destArgIndex = 0;
                    sink.sizeArgIndex = 2;
                    sink.displayName = "memcpy";
                    return sink;
                case Intrinsic::memset:
                    sink.valid = true;
                    sink.hasExplicitLength = true;
                    sink.destArgIndex = 0;
                    sink.sizeArgIndex = 2;
                    sink.displayName = "memset";
                    return sink;
                case Intrinsic::memmove:
                    sink.valid = true;
                    sink.hasExplicitLength = true;
                    sink.destArgIndex = 0;
                    sink.sizeArgIndex = 2;
                    sink.displayName = "memmove";
                    return sink;
                default:
                    break;
                }
            }

            Function* callee = resolveDirectCallee(CB);
            if (!callee)
                return sink;

            StringRef calleeName = callee->getName();
            if (calleeName == "memcpy" || calleeName == "__memcpy_chk")
            {
                sink.valid = true;
                sink.hasExplicitLength = true;
                sink.destArgIndex = 0;
                sink.sizeArgIndex = 2;
                sink.displayName = "memcpy";
                return sink;
            }
            if (calleeName == "memset" || calleeName == "__memset_chk")
            {
                sink.valid = true;
                sink.hasExplicitLength = true;
                sink.destArgIndex = 0;
                sink.sizeArgIndex = 2;
                sink.displayName = "memset";
                return sink;
            }
            if (calleeName == "memmove" || calleeName == "__memmove_chk")
            {
                sink.valid = true;
                sink.hasExplicitLength = true;
                sink.destArgIndex = 0;
                sink.sizeArgIndex = 2;
                sink.displayName = "memmove";
                return sink;
            }

            return sink;
        }

        static ResolvedSink resolveModelSink(llvm::CallBase* CB, const BufferWriteModel* model,
                                             BufferWriteRuleMatcher* matcher)
        {
            ResolvedSink sink;
            if (!CB || !model || !matcher)
                return sink;

            llvm::Function* callee = resolveDirectCallee(CB);
            if (!callee)
                return sink;

            const BufferWriteRule* rule =
                matcher->findMatchingRule(*model, *callee, CB->arg_size());
            if (!rule)
                return sink;

            sink.valid = true;
            sink.hasExplicitLength = (rule->kind == BufferWriteRuleKind::BoundedWrite);
            sink.destArgIndex = rule->destArgIndex;
            sink.sizeArgIndex = rule->sizeArgIndex;
            sink.displayName = callee->getName().str();
            return sink;
        }

        static const llvm::AllocaInst* resolveStackDestinationAlloca(llvm::Value* destinationPtr)
        {
            using namespace llvm;
            if (!destinationPtr)
                return nullptr;

            const Value* cur = destinationPtr->stripPointerCasts();
            if (auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                cur = GEP->getPointerOperand();
            return dyn_cast<AllocaInst>(cur);
        }

        static void analyzeMemIntrinsicOverflowsInFunction(llvm::Function& F,
                                                           const llvm::DataLayout& DL,
                                                           const BufferWriteModel* externalModel,
                                                           BufferWriteRuleMatcher* ruleMatcher,
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

                    ResolvedSink sink = resolveBuiltInSink(CB);
                    const ResolvedSink modeledSink =
                        resolveModelSink(CB, externalModel, ruleMatcher);
                    if (modeledSink.valid)
                        sink = modeledSink;

                    if (!sink.valid)
                        continue;

                    if (CB->arg_size() <= sink.destArgIndex)
                        continue;

                    Value* dest = CB->getArgOperand(sink.destArgIndex);
                    const AllocaInst* AI = resolveStackDestinationAlloca(dest);
                    if (!AI)
                        continue;

                    auto maybeSize = getAllocaTotalSizeBytes(AI, DL);
                    if (!maybeSize)
                        continue;
                    StackSize destBytes = *maybeSize;

                    MemIntrinsicIssue issue;
                    issue.funcName = F.getName().str();
                    issue.varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                    issue.destSizeBytes = destBytes;
                    issue.inst = &I;
                    issue.intrinsicName = sink.displayName;

                    if (sink.hasExplicitLength)
                    {
                        if (CB->arg_size() <= sink.sizeArgIndex)
                            continue;
                        Value* lenV = CB->getArgOperand(sink.sizeArgIndex);
                        auto* lenC = dyn_cast<ConstantInt>(lenV);
                        if (!lenC)
                            continue;

                        const uint64_t len = lenC->getZExtValue();
                        if (len <= destBytes)
                            continue;
                        issue.lengthBytes = len;
                        issue.hasExplicitLength = true;
                    }
                    else
                    {
                        issue.hasExplicitLength = false;
                    }

                    out.push_back(std::move(issue));
                }
            }
        }
    } // namespace

    std::vector<MemIntrinsicIssue>
    analyzeMemIntrinsicOverflows(llvm::Module& mod, const llvm::DataLayout& DL,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                 const std::string& bufferModelPath)
    {
        BufferWriteModel externalModel;
        BufferWriteRuleMatcher ruleMatcher;
        const BufferWriteModel* externalModelPtr = nullptr;
        if (!bufferModelPath.empty())
        {
            std::string parseError;
            if (!parseBufferWriteModel(bufferModelPath, externalModel, parseError))
            {
                std::cerr << "Buffer model load error: " << parseError << "\n";
            }
            else
            {
                externalModelPtr = &externalModel;
            }
        }

        std::vector<MemIntrinsicIssue> issues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeMemIntrinsicOverflowsInFunction(F, DL, externalModelPtr, &ruleMatcher, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
