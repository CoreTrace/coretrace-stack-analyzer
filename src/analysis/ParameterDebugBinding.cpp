#include "analysis/ParameterDebugBinding.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        enum class BindingOrigin : std::uint8_t
        {
            None = 0,
            DvrDeclare = 1,
            DbgUser = 2,
            DbgRecordUser = 3,
            DbgDeclare = 4,
            RetainedNode = 5
        };

        struct DebugParamLayout
        {
            const llvm::DISubroutineType* subroutineType = nullptr;
            bool hasCanonicalDebugIndex = false;
            bool isHiddenAbiArg = false;
            // Make alignment intent explicit before the 32-bit index.
            std::uint16_t paddingBeforeDebugArgIndex = 0;
            unsigned debugArgIndex = 0; // 1-based source-level parameter index.
        };

        struct VariableCandidate
        {
            const llvm::DILocalVariable* var = nullptr;
            llvm::DebugLoc loc;
            BindingOrigin origin = BindingOrigin::None;
            // Keep alignment explicit before score and at struct tail for -Wpadded builds.
            std::uint8_t paddingBeforeScore[3] = {};
            int score = std::numeric_limits<int>::min();
            bool argMatchesExpected = false;
            std::uint8_t paddingTail[7] = {};
        };

        static DebugParamLayout buildDebugParamLayout(const llvm::Function& F,
                                                      const llvm::Argument& Arg,
                                                      const llvm::DISubprogram* SP)
        {
            DebugParamLayout layout;
            if (!SP)
                return layout;

            const auto* subroutineType =
                llvm::dyn_cast_or_null<llvm::DISubroutineType>(SP->getType());
            if (!subroutineType)
                return layout;
            layout.subroutineType = subroutineType;

            const auto typeArray = subroutineType->getTypeArray();
            if (typeArray.size() < 2)
                return layout; // [0] return type, [1..] params.

            const unsigned irArgCount = static_cast<unsigned>(F.arg_size());
            const unsigned debugParamCount = static_cast<unsigned>(typeArray.size() - 1);
            if (irArgCount < debugParamCount)
                return layout;

            const unsigned hiddenPrefix = irArgCount - debugParamCount;
            if (Arg.getArgNo() < hiddenPrefix)
            {
                layout.isHiddenAbiArg = true;
                return layout;
            }

            const unsigned debugParamOffset = Arg.getArgNo() - hiddenPrefix;
            if (debugParamOffset >= debugParamCount)
                return layout;

            layout.debugArgIndex = debugParamOffset + 1;
            layout.hasCanonicalDebugIndex = true;
            return layout;
        }

        static bool hasSyntheticFlags(const llvm::DINode::DIFlags flags)
        {
            return (flags & llvm::DINode::FlagArtificial) ||
                   (flags & llvm::DINode::FlagObjectPointer);
        }

        static bool isSyntheticDebugVariable(const llvm::DILocalVariable* var)
        {
            if (!var)
                return false;
            return var->isArtificial() || var->isObjectPointer() ||
                   hasSyntheticFlags(var->getFlags());
        }

        static unsigned expectedDebugArgIndex(const llvm::Argument& Arg,
                                              const DebugParamLayout& layout)
        {
            if (layout.hasCanonicalDebugIndex)
                return layout.debugArgIndex;

            // Legacy fallback if canonical mapping cannot be computed.
            return Arg.getArgNo() + 1;
        }

        static int scoreVariable(const llvm::DILocalVariable* var, llvm::DebugLoc loc,
                                 BindingOrigin origin, const llvm::Argument& Arg,
                                 const DebugParamLayout& layout, bool& outArgMatchesExpected)
        {
            outArgMatchesExpected = false;
            if (!var || !var->isParameter())
                return std::numeric_limits<int>::min();

            const unsigned expectedArg = expectedDebugArgIndex(Arg, layout);
            outArgMatchesExpected = var->getArg() == expectedArg;

            std::uint64_t positiveScore = 0;
            std::uint64_t penaltyScore = 0;
            switch (origin)
            {
            case BindingOrigin::DvrDeclare:
                positiveScore += 60;
                break;
            case BindingOrigin::DbgUser:
                positiveScore += 55;
                break;
            case BindingOrigin::DbgRecordUser:
                positiveScore += 52;
                break;
            case BindingOrigin::DbgDeclare:
                positiveScore += 50;
                break;
            case BindingOrigin::RetainedNode:
                positiveScore += 30;
                break;
            case BindingOrigin::None:
                break;
            }

            if (outArgMatchesExpected)
                positiveScore += 120;
            else
                penaltyScore += 90;
            if (var->getType())
                positiveScore += 30;
            if (!var->getName().empty())
                positiveScore += 20;
            if (loc && loc.getLine() != 0)
                positiveScore += 8;
            else if (var->getLine() != 0)
                positiveScore += 4;
            if (isSyntheticDebugVariable(var))
                penaltyScore += 20;

            if (positiveScore >= penaltyScore)
            {
                const std::uint64_t delta = positiveScore - penaltyScore;
                if (delta > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
                    return std::numeric_limits<int>::max();
                return static_cast<int>(delta);
            }

            const std::uint64_t delta = penaltyScore - positiveScore;
            constexpr std::uint64_t kIntMinMagnitude =
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()) + 1ull;
            if (delta >= kIntMinMagnitude)
                return std::numeric_limits<int>::min();
            return -static_cast<int>(delta);
        }

        static void considerVariableCandidate(VariableCandidate& best,
                                              const llvm::DILocalVariable* var, llvm::DebugLoc loc,
                                              BindingOrigin origin, const llvm::Argument& Arg,
                                              const DebugParamLayout& layout)
        {
            bool argMatchesExpected = false;
            const int score = scoreVariable(var, loc, origin, Arg, layout, argMatchesExpected);
            if (score <= best.score)
                return;

            best.var = var;
            best.loc = loc;
            best.origin = origin;
            best.score = score;
            best.argMatchesExpected = argMatchesExpected;
        }

        static void applyVariableLineAndColumn(ParameterDebugBinding& binding,
                                               const VariableCandidate& candidate)
        {
            if (candidate.loc && candidate.loc.getLine() != 0)
            {
                binding.line = candidate.loc.getLine();
                binding.column = candidate.loc.getCol();
                if (binding.column == 0)
                    binding.column = 1;
                return;
            }

            if (candidate.var && candidate.var->getLine() != 0)
            {
                binding.line = candidate.var->getLine();
                binding.column = 0;
            }
        }

        static ParameterBindingConfidence
        confidenceFromCandidate(const VariableCandidate& candidate)
        {
            if (!candidate.var || !candidate.var->getType())
                return ParameterBindingConfidence::Low;
            if (!candidate.argMatchesExpected)
                return ParameterBindingConfidence::Low;
            if (candidate.var->getName().empty())
                return ParameterBindingConfidence::Low;

            if (candidate.origin == BindingOrigin::RetainedNode)
                return ParameterBindingConfidence::Medium;
            return ParameterBindingConfidence::High;
        }

        static void finalizeBindingName(ParameterDebugBinding& binding, const llvm::Argument& Arg)
        {
            if (binding.name.empty())
                binding.name = Arg.getName().str();
            if (binding.name.empty())
            {
                binding.isAnonymous = true;
                binding.name = "param" + std::to_string(Arg.getArgNo() + 1);
            }
            else
            {
                binding.isAnonymous = false;
            }
        }
    } // namespace

    ParameterDebugBinding resolveParameterDebugBinding(const llvm::Function& F,
                                                       const llvm::Argument& Arg)
    {
        ParameterDebugBinding binding;
        binding.name = Arg.getName().str();

        const llvm::DISubprogram* SP = F.getSubprogram();
        const DebugParamLayout layout = buildDebugParamLayout(F, Arg, SP);
        if (layout.isHiddenAbiArg)
            binding.isArtificial = true;

        VariableCandidate best;

        auto* nonConstArg = const_cast<llvm::Argument*>(&Arg);

        for (llvm::DbgVariableRecord* dvr : llvm::findDVRDeclares(nonConstArg))
        {
            considerVariableCandidate(best, dvr ? dvr->getVariable() : nullptr,
                                      llvm::getDebugValueLoc(dvr), BindingOrigin::DvrDeclare, Arg,
                                      layout);
        }

        llvm::SmallVector<llvm::DbgVariableIntrinsic*, 4> dbgUsers;
        llvm::SmallVector<llvm::DbgVariableRecord*, 4> dbgRecords;
        llvm::findDbgUsers(dbgUsers, nonConstArg, &dbgRecords);

        for (llvm::DbgVariableIntrinsic* dvi : dbgUsers)
        {
            considerVariableCandidate(best, dvi ? dvi->getVariable() : nullptr,
                                      llvm::getDebugValueLoc(dvi), BindingOrigin::DbgUser, Arg,
                                      layout);
        }

        for (llvm::DbgVariableRecord* dvr : dbgRecords)
        {
            considerVariableCandidate(best, dvr ? dvr->getVariable() : nullptr,
                                      llvm::getDebugValueLoc(dvr), BindingOrigin::DbgRecordUser,
                                      Arg, layout);
        }

        for (llvm::DbgDeclareInst* ddi : llvm::findDbgDeclares(nonConstArg))
        {
            considerVariableCandidate(best, ddi ? ddi->getVariable() : nullptr,
                                      llvm::getDebugValueLoc(ddi), BindingOrigin::DbgDeclare, Arg,
                                      layout);
        }

        if (SP)
        {
            for (llvm::DINode* node : SP->getRetainedNodes())
            {
                const auto* var = llvm::dyn_cast_or_null<llvm::DILocalVariable>(node);
                if (!var)
                    continue;
                considerVariableCandidate(best, var, llvm::DebugLoc{}, BindingOrigin::RetainedNode,
                                          Arg, layout);
            }
        }

        if (best.var)
        {
            if (!best.var->getName().empty())
                binding.name = best.var->getName().str();
            binding.type = best.var->getType();
            binding.isArtificial |= isSyntheticDebugVariable(best.var);
            binding.confidence = confidenceFromCandidate(best);
            applyVariableLineAndColumn(binding, best);
        }

        if (!binding.type && layout.hasCanonicalDebugIndex && layout.subroutineType)
        {
            const auto typeArray = layout.subroutineType->getTypeArray();
            if (layout.debugArgIndex < typeArray.size())
            {
                if (const auto* paramType =
                        llvm::dyn_cast_or_null<llvm::DIType>(typeArray[layout.debugArgIndex]))
                {
                    binding.type = paramType;
                    binding.isArtificial |= hasSyntheticFlags(paramType->getFlags());
                }
            }
            binding.confidence = ParameterBindingConfidence::Low;
        }

        if (SP && binding.line == 0 && SP->getLine() != 0)
        {
            binding.line = SP->getLine();
        }

        finalizeBindingName(binding, Arg);
        return binding;
    }
} // namespace ctrace::stack::analysis
