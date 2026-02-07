#include "analysis/DynamicAlloca.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/IRValueUtils.hpp"

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

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;

                    // Taille d'allocation : on distingue trois cas :
                    //  - constante immédiate               -> pas une VLA
                    //  - dérivée d'une constante simple    -> pas une VLA (heuristique)
                    //  - vraiment dépendante d'une valeur  -> VLA / alloca variable
                    Value* arraySizeVal = AI->getArraySize();

                    // 1) Cas taille directement constante dans l'IR
                    if (llvm::isa<llvm::ConstantInt>(arraySizeVal))
                        continue; // taille connue à la compilation, OK

                    // 2) Heuristique "smart" : essayer de remonter à une constante
                    //    via les stores dans une variable locale (tryGetConstFromValue).
                    //    Exemple typique :
                    //      int n = 6;
                    //      char buf[n];   // en C : VLA, mais ici n est en fait constant
                    //
                    //    Dans ce cas, on ne veut pas spammer avec un warning VLA :
                    //    on traite ça comme une taille effectivement constante.
                    if (tryGetConstFromValue(arraySizeVal, F) != nullptr)
                        continue;

                    // 3) Ici, on considère que c'est une vraie VLA / alloca dynamique
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
} // namespace ctrace::stack::analysis
