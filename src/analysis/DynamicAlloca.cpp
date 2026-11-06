// SPDX-License-Identifier: Apache-2.0
#include "analysis/DynamicAlloca.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/FunctionFacts.hpp"
#include "analysis/IRValueUtils.hpp"

#include <optional>

namespace ctrace::stack::analysis
{
    namespace
    {
        static void analyzeDynamicAllocasInFunction(llvm::Function& F,
                                                    std::vector<DynamicAllocaIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            // Built on the first non-constant size only: most functions have no VLA.
            std::optional<FunctionFacts> facts;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;

                    // Allocation size: we distinguish three cases:
                    //  - immediate constant               -> not a VLA
                    //  - derived from a simple constant   -> not a VLA (heuristic)
                    //  - truly value-dependent            -> VLA / variable alloca
                    Value* arraySizeVal = AI->getArraySize();

                    // 1) Size is directly constant in the IR
                    if (llvm::isa<llvm::ConstantInt>(arraySizeVal))
                        continue; // compile-time known size, OK

                    // 2) The size is a constant stored into a local variable, read back
                    //    unchanged (tryGetConstFromValue). Typical example:
                    //      int n = 6;
                    //      char buf[n];   // in C: VLA, but here n is actually constant
                    //
                    //    In this case we don't want to spam with a VLA warning:
                    //    treat it as an effectively constant size.
                    if (tryGetConstFromValue(arraySizeVal, facts ? *facts : facts.emplace(F)) !=
                        nullptr)
                        continue;

                    // 3) Here we consider it a real VLA / dynamic alloca
                    DynamicAllocaIssue issue;
                    issue.funcName = F.getName().str();
                    issue.varName = deriveAllocaName(AI);
                    if (AI->getAllocatedType())
                    {
                        std::string tyStr;
                        llvm::raw_string_ostream rso(tyStr);
                        AI->getAllocatedType()->print(rso);
                        issue.typeName = rso.str();
                    }
                    else
                    {
                        issue.typeName = "<unknown type>";
                    }
                    issue.allocaInst = AI;
                    out.push_back(std::move(issue));
                }
            }
        }
    } // namespace

    std::vector<DynamicAllocaIssue>
    analyzeDynamicAllocas(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<DynamicAllocaIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeDynamicAllocasInFunction(F, out);
        }

        return out;
    }

    std::vector<DynamicAllocaIssue>
    analyzeDynamicAllocasCached(const llvm::Function& function,
                                const std::vector<const llvm::AllocaInst*>& allocas)
    {
        std::vector<DynamicAllocaIssue> out;
        using namespace llvm;
        std::optional<FunctionFacts> facts;

        for (const AllocaInst* AI : allocas)
        {
            const Value* arraySizeVal = AI->getArraySize();

            if (llvm::isa<llvm::ConstantInt>(arraySizeVal))
                continue;

            // FunctionFacts needs a mutable function for its analyses; they do not change it.
            if (!facts)
                facts.emplace(const_cast<Function&>(function));
            if (tryGetConstFromValue(arraySizeVal, *facts) != nullptr)
                continue;

            DynamicAllocaIssue issue;
            issue.funcName = function.getName().str();
            issue.varName = deriveAllocaName(AI);
            if (AI->getAllocatedType())
            {
                std::string tyStr;
                llvm::raw_string_ostream rso(tyStr);
                AI->getAllocatedType()->print(rso);
                issue.typeName = rso.str();
            }
            else
            {
                issue.typeName = "<unknown type>";
            }
            issue.allocaInst = AI;
            out.push_back(std::move(issue));
        }

        return out;
    }
} // namespace ctrace::stack::analysis
