#include "analyzer/PerFunctionInstructionCache.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

namespace ctrace::stack::analyzer
{
    void PerFunctionInstructionCache::onFunctionBegin(const llvm::Function& F)
    {
        currentFunction_ = &F;
        currentData_ = {};
    }

    void PerFunctionInstructionCache::onFunctionEnd(const llvm::Function& F)
    {
        if (currentFunction_ == &F)
            data_.try_emplace(&F, std::move(currentData_));
        currentFunction_ = nullptr;
        currentData_ = {};
    }

    void PerFunctionInstructionCache::onAlloca(const llvm::AllocaInst& inst)
    {
        currentData_.allocas.push_back(&inst);
    }

    void PerFunctionInstructionCache::onLoad(const llvm::LoadInst& inst)
    {
        currentData_.loads.push_back(&inst);
    }

    void PerFunctionInstructionCache::onStore(const llvm::StoreInst& inst)
    {
        currentData_.stores.push_back(&inst);
    }

    void PerFunctionInstructionCache::onCall(const llvm::CallInst& inst)
    {
        currentData_.calls.push_back(&inst);
    }

    void PerFunctionInstructionCache::onInvoke(const llvm::InvokeInst& inst)
    {
        currentData_.invokes.push_back(&inst);
    }

    void PerFunctionInstructionCache::onMemIntrinsic(const llvm::MemIntrinsic& inst)
    {
        currentData_.memIntrinsics.push_back(&inst);
    }
} // namespace ctrace::stack::analyzer
