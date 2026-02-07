#pragma once

#include <functional>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class AllocaInst;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct StackBufferOverflowIssue
    {
        std::string funcName;
        std::string varName;
        StackSize arraySize = 0;
        StackSize indexOrUpperBound = 0; // utilisé pour les bornes sup (UB) ou index constant
        bool isWrite = false;
        bool indexIsConstant = false;
        const llvm::Instruction* inst = nullptr;

        // Violation basée sur une borne inférieure (index potentiellement négatif)
        bool isLowerBoundViolation = false;
        long long lowerBound = 0; // borne inférieure déduite (signée)

        std::string aliasPath;                 // ex: "pp -> ptr -> buf"
        std::vector<std::string> aliasPathVec; // {"pp", "ptr", "buf"}
        // Optional : helper for sync string <- vector
        void rebuildAliasPathString(const std::string& sep = " -> ")
        {
            aliasPath.clear();
            for (size_t i = 0; i < aliasPathVec.size(); ++i)
            {
                aliasPath += aliasPathVec[i];
                if (i + 1 < aliasPathVec.size())
                    aliasPath += sep;
            }
        }
    };

    struct MultipleStoreIssue
    {
        std::string funcName;
        std::string varName;
        std::size_t storeCount = 0;         // nombre total de StoreInst vers ce buffer
        std::size_t distinctIndexCount = 0; // nombre d'expressions d'index distinctes
        const llvm::AllocaInst* allocaInst = nullptr;
    };

    std::vector<StackBufferOverflowIssue>
    analyzeStackBufferOverflows(llvm::Module& mod,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    std::vector<MultipleStoreIssue>
    analyzeMultipleStores(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
