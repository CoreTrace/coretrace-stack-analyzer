#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Argument;
    class CallBase;
    class Function;
    class FunctionType;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class EscapeSummaryState
    {
        Unknown,
        NoEscape,
        MayEscape
    };

    struct StackEscapeRule
    {
        std::string functionPattern;
        unsigned argIndex = 0;
    };

    struct StackEscapeModel
    {
        std::vector<StackEscapeRule> noEscapeArgRules;
    };

    struct DirectParamDependency
    {
        const llvm::Function* callee = nullptr;
        unsigned argIndex = 0;
    };

    struct IndirectCallDependency
    {
        std::vector<DirectParamDependency> candidates;
        bool hasUnknownTarget = false;
    };

    struct ParamEscapeFacts
    {
        bool hardEscape = false;
        bool hasOpaqueExternalCall = false;
        std::vector<DirectParamDependency> directDeps;
        std::vector<IndirectCallDependency> indirectDeps;
    };

    struct FunctionEscapeFacts
    {
        std::vector<ParamEscapeFacts> perArg;
    };

    using FunctionEscapeSummaryMap =
        std::unordered_map<const llvm::Function*, std::vector<EscapeSummaryState>>;
    using FunctionEscapeFactsMap = std::unordered_map<const llvm::Function*, FunctionEscapeFacts>;
    using ReturnedPointerArgAliasMap = std::unordered_map<const llvm::Function*, unsigned>;

    class StackEscapeRuleMatcher
    {
      public:
        bool modelSaysNoEscapeArg(const StackEscapeModel& model, const llvm::Function* callee,
                                  unsigned argIndex);

      private:
        struct NameVariants
        {
            std::string mangled;
            std::string demangled;
            std::string demangledBase;
        };

        bool modelRuleMatchesFunction(const StackEscapeRule& rule, const llvm::Function& callee);
        const NameVariants& namesFor(const llvm::Function& callee);

        std::unordered_map<const llvm::Function*, NameVariants> namesCache;
    };

    class IndirectTargetResolver
    {
      public:
        explicit IndirectTargetResolver(const llvm::Module& module);
        const std::vector<const llvm::Function*>& candidatesForCall(const llvm::CallBase& CB) const;

      private:
        std::unordered_map<const llvm::FunctionType*, std::vector<const llvm::Function*>>
            candidatesByFunctionType;
        std::vector<const llvm::Function*> empty;
    };

    bool parseStackEscapeModel(const std::string& path, StackEscapeModel& out, std::string& error);
    bool callParamHasNonCaptureLikeAttr(const llvm::CallBase& CB, unsigned argIndex);
    bool isStdLibCallee(const llvm::Function* F);
    bool isLikelyVirtualDispatchCall(const llvm::CallBase& CB);
} // namespace ctrace::stack::analysis
