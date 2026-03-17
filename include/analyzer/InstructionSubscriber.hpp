#pragma once

#include <vector>

namespace llvm
{
    class AllocaInst;
    class CallInst;
    class Function;
    class InvokeInst;
    class LoadInst;
    class MemIntrinsic;
    class StoreInst;
} // namespace llvm

namespace ctrace::stack::analyzer
{
    class InstructionSubscriber
    {
      public:
        virtual ~InstructionSubscriber() = default;

        virtual void onFunctionBegin(const llvm::Function&) {}
        virtual void onFunctionEnd(const llvm::Function&) {}
        virtual void onAlloca(const llvm::AllocaInst&) {}
        virtual void onLoad(const llvm::LoadInst&) {}
        virtual void onStore(const llvm::StoreInst&) {}
        virtual void onCall(const llvm::CallInst&) {}
        virtual void onInvoke(const llvm::InvokeInst&) {}
        virtual void onMemIntrinsic(const llvm::MemIntrinsic&) {}
    };

    class InstructionSubscriberRegistry
    {
      public:
        void add(InstructionSubscriber& subscriber) { subscribers_.push_back(&subscriber); }

        [[nodiscard]] bool empty() const { return subscribers_.empty(); }

        void notifyFunctionBegin(const llvm::Function& F) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onFunctionBegin(F);
        }

        void notifyFunctionEnd(const llvm::Function& F) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onFunctionEnd(F);
        }

        void notifyAlloca(const llvm::AllocaInst& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onAlloca(inst);
        }

        void notifyLoad(const llvm::LoadInst& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onLoad(inst);
        }

        void notifyStore(const llvm::StoreInst& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onStore(inst);
        }

        void notifyCall(const llvm::CallInst& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onCall(inst);
        }

        void notifyInvoke(const llvm::InvokeInst& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onInvoke(inst);
        }

        void notifyMemIntrinsic(const llvm::MemIntrinsic& inst) const
        {
            for (InstructionSubscriber* subscriber : subscribers_)
                subscriber->onMemIntrinsic(inst);
        }

      private:
        std::vector<InstructionSubscriber*> subscribers_;
    };
} // namespace ctrace::stack::analyzer
