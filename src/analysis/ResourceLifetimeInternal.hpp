// SPDX-License-Identifier: Apache-2.0
// Storage/handle resolution shared by ResourceLifetimeAnalysis.cpp and the
// ownership fact collector. Internal to src/analysis; not part of the public API.
#pragma once

#include <cstdint>
#include <string>

namespace llvm
{
    class AllocaInst;
    class CallBase;
    class DataLayout;
    class Function;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis::lifetime_detail
{
    struct MethodClassInfo
    {
        std::string className;
        std::string methodName;
        std::uint64_t isCtor : 1 = false;
        std::uint64_t isDtor : 1 = false;
        std::uint64_t isLifecycleReleaseLike : 1 = false;
        std::uint64_t reservedFlags : 61 = 0;
    };

    enum class StorageScope
    {
        Unknown,
        Local,
        Global,
        Argument,
        ThisField
    };

    struct StorageKey
    {
        std::string key;
        std::string displayName;
        std::string className;
        const llvm::AllocaInst* localAlloca = nullptr;
        std::uint64_t offset = 0;
        int argumentIndex = -1;
        StorageScope scope = StorageScope::Unknown;

        bool valid() const
        {
            return scope != StorageScope::Unknown && !key.empty();
        }
    };

    MethodClassInfo describeMethodClass(const llvm::Function& F);
    StorageKey resolvePointerStorage(const llvm::Value* ptr, const llvm::Function& F,
                                     const llvm::DataLayout& DL, const MethodClassInfo& methodInfo);
    StorageKey resolveHandleStorage(const llvm::Value* handleValue, const llvm::Function& F,
                                    const llvm::DataLayout& DL, const MethodClassInfo& methodInfo);
    const llvm::Function* resolveDirectCallee(const llvm::CallBase& CB);
} // namespace ctrace::stack::analysis::lifetime_detail
