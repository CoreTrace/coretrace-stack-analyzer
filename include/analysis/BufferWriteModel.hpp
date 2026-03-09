#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class BufferWriteRuleKind : std::uint64_t
    {
        BoundedWrite = 0,
        UnboundedWrite = 1
    };

    struct BufferWriteRule
    {
        std::string functionPattern;
        unsigned destArgIndex = 0;
        unsigned sizeArgIndex = 0; // only used for BoundedWrite
        BufferWriteRuleKind kind = BufferWriteRuleKind::BoundedWrite;
    };

    struct BufferWriteModel
    {
        std::vector<BufferWriteRule> rules;
    };

    class BufferWriteRuleMatcher
    {
      public:
        const BufferWriteRule* findMatchingRule(const BufferWriteModel& model,
                                                const llvm::Function& callee, std::size_t argCount);

      private:
        struct NameVariants
        {
            std::string mangled;
            std::string demangled;
            std::string demangledBase;
        };

        const NameVariants& namesFor(const llvm::Function& callee);
        bool ruleMatchesFunction(const BufferWriteRule& rule, const llvm::Function& callee);

        std::unordered_map<const llvm::Function*, NameVariants> namesCache;
    };

    bool parseBufferWriteModel(const std::string& path, BufferWriteModel& out, std::string& error);
} // namespace ctrace::stack::analysis
