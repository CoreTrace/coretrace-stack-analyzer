#pragma once

#include "analyzer/InstructionSubscriber.hpp"

#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace llvm
{
    class Function;
} // namespace llvm

namespace ctrace::stack::analyzer
{
    struct PerFunctionData
    {
        std::vector<const llvm::AllocaInst*> allocas;
        std::vector<const llvm::StoreInst*> stores;
        std::vector<const llvm::CallInst*> calls;
        std::vector<const llvm::InvokeInst*> invokes;
        std::vector<const llvm::LoadInst*> loads;
        std::vector<const llvm::MemIntrinsic*> memIntrinsics;
    };

    class PerFunctionInstructionCache : public InstructionSubscriber
    {
      public:
        void onFunctionBegin(const llvm::Function& F) override;
        void onFunctionEnd(const llvm::Function& F) override;
        void onAlloca(const llvm::AllocaInst& inst) override;
        void onLoad(const llvm::LoadInst& inst) override;
        void onStore(const llvm::StoreInst& inst) override;
        void onCall(const llvm::CallInst& inst) override;
        void onInvoke(const llvm::InvokeInst& inst) override;
        void onMemIntrinsic(const llvm::MemIntrinsic& inst) override;

        [[nodiscard]] const llvm::DenseMap<const llvm::Function*, PerFunctionData>& data() const
        {
            return data_;
        }

      private:
        llvm::DenseMap<const llvm::Function*, PerFunctionData> data_;
        const llvm::Function* currentFunction_ = nullptr;
        PerFunctionData currentData_;
    };
} // namespace ctrace::stack::analyzer
