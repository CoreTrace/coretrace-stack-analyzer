// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <llvm/ADT/StringRef.h>

namespace llvm
{
    class AllocaInst;
    class CallBase;
    class ConstantInt;
    class Function;
    class StoreInst;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis
{
    class FunctionFacts;

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

    /// The function a call reaches directly, through pointer casts and aliases: on ELF targets,
    /// clang emits a complete-object constructor or destructor defined out of its class as an
    /// alias of the base-object one, and calls the alias. Null for an indirect call, and for a
    /// call through an alias that the link may replace (weak): its target is not known.
    llvm::Function* directCallee(const llvm::CallBase& call);

    std::string deriveAllocaName(const llvm::AllocaInst* AI);

    bool isLikelyCompilerTemporaryName(llvm::StringRef name);

    AllocaOrigin classifyAllocaOrigin(const llvm::AllocaInst* AI);

    /// The integer constant @p V holds wherever it is used, or null when that is not established.
    ///
    /// Follows casts, applying them to the constant, and loads whose MemorySSA clobber is a
    /// store of the same type to the same pointer, so the store wrote exactly the bytes the load
    /// reads. Any other clobber, such as a merge of several stores, a store that may only
    /// overlap, a call or the function entry, gives null.
    const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V, const FunctionFacts& facts);

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
