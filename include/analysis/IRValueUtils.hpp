// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <llvm/ADT/StringRef.h>

namespace llvm
{
    class AllocaInst;
    class ConstantInt;
    class Function;
    class StoreInst;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class LeadingUnderscores
    {
        One,
        All
    };

    /// Normalize an external symbol for name-based models, returning a view into name.
    /// Removes LLVM's no-mangling marker, leading underscores according to the caller's
    /// policy, and the suffix beginning at the first '$'.
    llvm::StringRef
    canonicalExternalCalleeName(llvm::StringRef name,
                                LeadingUnderscores underscores = LeadingUnderscores::One);

    enum class AllocaOrigin
    {
        User,
        CompilerGenerated,
        Unknown
    };

    std::string deriveAllocaName(const llvm::AllocaInst* AI);

    bool isLikelyCompilerTemporaryName(llvm::StringRef name);

    AllocaOrigin classifyAllocaOrigin(const llvm::AllocaInst* AI);

    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V, const llvm::Function& F);

    /// The sole store to a slot whose other uses are loads, debug or lifetime intrinsics.
    /// Returns null for multiple stores, address escapes or unsupported uses.
    const llvm::StoreInst* findUniqueStoreToSlot(const llvm::AllocaInst& slot);

    /// Follow reloads of static pointer slots under the strict single-store contract.
    /// Keeps the original value when nothing can be peeled (including its casts).
    /// The depth and value check preserve the existing callers' precision policies;
    /// some provenance analyses also follow non-pointer stores to pointer-sized slots.
    const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* value,
                                                      unsigned maxDepth = 4,
                                                      bool requirePointerValue = true);

    /// Resolve a pointer shadow slot. Unlike strict peeling, storing the slot's address
    /// is allowed; requireSimpleUses=false also permits other address users.
    const llvm::Value* resolveAllocaPointerShadowValue(const llvm::AllocaInst& slot,
                                                       bool requireSimpleUses);
} // namespace ctrace::stack::analysis
