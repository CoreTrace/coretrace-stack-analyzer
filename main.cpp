// // #include <llvm/IR/LLVMContext.h>
// // #include <llvm/IR/Module.h>
// // #include <llvm/IR/Function.h>
// // #include <llvm/IR/Instructions.h>
// // #include <llvm/IR/Type.h>
// // #include <llvm/Support/SourceMgr.h>
// // #include <llvm/IRReader/IRReader.h>
// // #include <llvm/Support/raw_ostream.h>
// // #include <llvm/Support/CommandLine.h>
// // #include <llvm/Support/FileSystem.h>
// // #include <llvm/IR/Constants.h>


// // int main(int argc, char **argv)
// // {
// //     llvm::LLVMContext context;
// //     llvm::SMDiagnostic err;

// //     if (argc < 2)
// //     {
// //         llvm::errs() << "Usage: stack_usage_analyzer <file.ll>\n";
// //         return 1;
// //     }

// //     std::unique_ptr<llvm::Module> mod = parseIRFile(argv[1], err, context);
// //     if (!mod)
// //     {
// //         err.print("stack_usage_analyzer", llvm::errs());
// //         return 1;
// //     }

// //     for (llvm::Function &F : *mod)
// //     {
// //         if (F.isDeclaration()) continue;

// //         uint64_t totalStack = 0;
// //         for (llvm::BasicBlock &BB : F)
// //         {
// //             for (llvm::Instruction &I : BB)
// //             {
// //                 if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I))
// //                 {
// //                     llvm::Type *ty = alloca->getAllocatedType();
// //                     uint64_t count = 1;

// //                     if (llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize()))
// //                     {
// //                         count = CI->getZExtValue();
// //                     }

// //                     auto &DL = mod->getDataLayout();
// //                     uint64_t size = DL.getTypeAllocSize(ty) * count;
// //                     totalStack += size;
// //                 }
// //             }
// //         }

// //         llvm::outs() << F.getName() << ": " << totalStack << " bytes\n";
// //     }

// //     return 0;
// // }

// #include <cstdint>
// #include <memory>
// #include <map>
// #include <vector>
// #include <set>
// #include <string>

// #include <llvm/IR/LLVMContext.h>
// #include <llvm/IR/Module.h>
// #include <llvm/IR/Function.h>
// #include <llvm/IR/Instructions.h>
// #include <llvm/IR/Type.h>
// #include <llvm/IR/Constants.h>
// // #include <llvm/IR/CallBase.h>
// #include <llvm/IRReader/IRReader.h>
// #include <llvm/Support/SourceMgr.h>
// #include <llvm/Support/raw_ostream.h>

// // Type pour représenter une taille de pile
// using StackSize = std::uint64_t;

// // Graphe d'appels : F -> liste de fonctions appelées par F
// using CallGraph = std::map<const llvm::Function*, std::vector<const llvm::Function*>>;

// // ============================================================================
// //  Analyse locale de la stack par fonction
// // ============================================================================

// static StackSize computeLocalStack(llvm::Function &F, const llvm::DataLayout &DL)
// {
//     StackSize totalStack = 0;

//     for (llvm::BasicBlock &BB : F) {
//         for (llvm::Instruction &I : BB) {
//             if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
//                 llvm::Type *ty = alloca->getAllocatedType();
//                 StackSize count = 1;

//                 if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize())) {
//                     count = CI->getZExtValue();
//                 } else {
//                     // Alloca dynamique (taille non constante) : on avertit
//                     llvm::errs() << "Warning: dynamic alloca in function "
//                                  << F.getName() << "\n";
//                 }

//                 StackSize size = DL.getTypeAllocSize(ty) * count;
//                 totalStack += size;
//             }
//         }
//     }
//     return totalStack;
// }

// // ============================================================================
// //  Construction du graphe d'appels
// // ============================================================================

// static CallGraph buildCallGraph(llvm::Module &M)
// {
//     CallGraph CG;

//     for (llvm::Function &F : M) {
//         if (F.isDeclaration())
//             continue;

//         auto &vec = CG[&F];

//         for (llvm::BasicBlock &BB : F) {
//             for (llvm::Instruction &I : BB) {

//                 const llvm::Function *Callee = nullptr;

//                 // Support anciens LLVM : CallInst
//                 if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
//                     Callee = CI->getCalledFunction();
//                 }
//                 // Support InvokeInst (LLVM exceptions)
//                 else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
//                     Callee = II->getCalledFunction();
//                 }

//                 if (Callee && !Callee->isDeclaration()) {
//                     vec.push_back(Callee);
//                 }
//             }
//         }
//     }

//     return CG;
// }


// // ============================================================================
// //  Propagation de la stack + détection de cycles (récursivité)
// // ============================================================================

// enum VisitState { NotVisited = 0, Visiting = 1, Visited = 2 };

// struct AnalysisResult {
//     std::map<const llvm::Function*, StackSize> TotalStack;    // stack max, callees inclus
//     std::set<const llvm::Function*> RecursiveFuncs;           // fonctions dans au moins un cycle
// };

// // DFS pour calculer la conso de stack globale d'une fonction
// static StackSize dfsComputeStack(
//     const llvm::Function *F,
//     const CallGraph &CG,
//     const std::map<const llvm::Function*, StackSize> &LocalStack,
//     std::map<const llvm::Function*, VisitState> &State,
//     AnalysisResult &Res)
// {
//     auto itState = State.find(F);
//     if (itState != State.end()) {
//         if (itState->second == Visiting) {
//             // Cycle détecté : on marque tous les noeuds actuellement en "Visiting"
//             for (auto &p : State) {
//                 if (p.second == Visiting) {
//                     Res.RecursiveFuncs.insert(p.first);
//                 }
//             }
//             // On renvoie au moins la stack locale comme borne inférieure
//             auto itLocal = LocalStack.find(F);
//             return (itLocal != LocalStack.end()) ? itLocal->second : 0;
//         } else if (itState->second == Visited) {
//             // Déjà calculé
//             auto itTotal = Res.TotalStack.find(F);
//             return (itTotal != Res.TotalStack.end()) ? itTotal->second : 0;
//         }
//     }

//     // Marquer comme en cours
//     State[F] = Visiting;

//     auto itLocal = LocalStack.find(F);
//     StackSize local = (itLocal != LocalStack.end()) ? itLocal->second : 0;
//     StackSize maxCallee = 0;

//     auto itCG = CG.find(F);
//     if (itCG != CG.end()) {
//         for (const llvm::Function *Callee : itCG->second) {
//             StackSize calleeStack = dfsComputeStack(Callee, CG, LocalStack, State, Res);
//             if (calleeStack > maxCallee)
//                 maxCallee = calleeStack;
//         }
//     }

//     StackSize total = local + maxCallee;
//     Res.TotalStack[F] = total;
//     State[F] = Visited;
//     return total;
// }

// static AnalysisResult computeGlobalStackUsage(
//     const CallGraph &CG,
//     const std::map<const llvm::Function*, StackSize> &LocalStack)
// {
//     AnalysisResult Res;
//     std::map<const llvm::Function*, VisitState> State;

//     // Initialisation explicite (optionnel, NotVisited = 0)
//     for (auto &p : LocalStack) {
//         State[p.first] = NotVisited;
//     }

//     for (auto &p : LocalStack) {
//         const llvm::Function *F = p.first;
//         if (State[F] == NotVisited) {
//             dfsComputeStack(F, CG, LocalStack, State, Res);
//         }
//     }

//     return Res;
// }

// // ============================================================================
// //  main
// // ============================================================================

// int main(int argc, char **argv)
// {
//     llvm::LLVMContext context;
//     llvm::SMDiagnostic err;

//     if (argc < 2) {
//         llvm::errs() << "Usage: stack_usage_analyzer <file.ll>\n";
//         return 1;
//     }

//     std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(argv[1], err, context);
//     if (!mod) {
//         err.print("stack_usage_analyzer", llvm::errs());
//         return 1;
//     }

//     const llvm::DataLayout &DL = mod->getDataLayout();

//     // 1) Stack locale par fonction
//     std::map<const llvm::Function*, StackSize> LocalStack;
//     for (llvm::Function &F : *mod) {
//         if (F.isDeclaration())
//             continue;
//         StackSize sz = computeLocalStack(F, DL);
//         LocalStack[&F] = sz;
//     }

//     // 2) Graphe d'appels
//     CallGraph CG = buildCallGraph(*mod);

//     // 3) Propagation + détection de récursivité
//     AnalysisResult Res = computeGlobalStackUsage(CG, LocalStack);

//     // Limite de stack "thread" (exemple : 8 MiB)
//     const StackSize StackLimit = 8ull * 1024ull * 1024ull;

//     // 4) Affichage des résultats
//     for (llvm::Function &F : *mod) {
//         if (F.isDeclaration())
//             continue;

//         const llvm::Function *Fn = &F;

//         StackSize local = 0;
//         StackSize total = 0;

//         auto itLocal = LocalStack.find(Fn);
//         if (itLocal != LocalStack.end())
//             local = itLocal->second;

//         auto itTotal = Res.TotalStack.find(Fn);
//         if (itTotal != Res.TotalStack.end())
//             total = itTotal->second;

//         llvm::outs() << "Function: " << F.getName() << "\n";
//         llvm::outs() << "  local stack: " << local << " bytes\n";
//         llvm::outs() << "  max stack (including callees): " << total << " bytes\n";

//         if (Res.RecursiveFuncs.count(Fn)) {
//             llvm::outs() << "  [!] recursive or mutually recursive function detected\n";
//         }

//         if (total > StackLimit) {
//             llvm::outs() << "  [!] potential stack overflow: exceeds limit of "
//                          << StackLimit << " bytes\n";
//         }

//         llvm::outs() << "\n";
//     }

//     return 0;
// }

#include <cstdint>
#include <memory>
#include <map>
#include <vector>
#include <set>
#include <string>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Dominators.h>

// Type pour représenter une taille de pile
using StackSize = std::uint64_t;

// Graphe d'appels : F -> liste de fonctions appelées par F
using CallGraph = std::map<const llvm::Function*, std::vector<const llvm::Function*>>;

// ============================================================================
//  Analyse locale de la stack par fonction
// ============================================================================

static StackSize computeLocalStack(llvm::Function &F, const llvm::DataLayout &DL)
{
    StackSize totalStack = 0;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                llvm::Type *ty = alloca->getAllocatedType();
                StackSize count = 1;

                if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize())) {
                    count = CI->getZExtValue();
                } else {
                    // Alloca dynamique (taille non constante) : on avertit
                    llvm::errs() << "Warning: dynamic alloca in function "
                                 << F.getName() << "\n";
                }

                StackSize size = DL.getTypeAllocSize(ty) * count;
                totalStack += size;
            }
        }
    }
    return totalStack;
}

// ============================================================================
//  Construction du graphe d'appels
//  -> compatible avec toutes versions LLVM (CallInst / InvokeInst, pas CallBase)
// ============================================================================

static CallGraph buildCallGraph(llvm::Module &M)
{
    CallGraph CG;

    for (llvm::Function &F : M) {
        if (F.isDeclaration())
            continue;

        auto &vec = CG[&F]; // crée une entrée vide si besoin

        for (llvm::BasicBlock &BB : F) {
            for (llvm::Instruction &I : BB) {

                const llvm::Function *Callee = nullptr;

                if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                    Callee = CI->getCalledFunction();
                } else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                    Callee = II->getCalledFunction();
                }

                // Appels indirects : getCalledFunction() == nullptr -> ignorés pour l'instant
                if (Callee && !Callee->isDeclaration()) {
                    vec.push_back(Callee);
                }
            }
        }
    }

    return CG;
}

// ============================================================================
//  Propagation de la stack + détection de cycles (récursivité)
// ============================================================================

enum VisitState { NotVisited = 0, Visiting = 1, Visited = 2 };

struct AnalysisResult {
    std::map<const llvm::Function*, StackSize> TotalStack;    // stack max, callees inclus
    std::set<const llvm::Function*> RecursiveFuncs;           // fonctions dans au moins un cycle
    std::set<const llvm::Function*> InfiniteRecursionFuncs;   // auto-récursion “infinie” détectée
};

// DFS pour calculer la conso de stack globale d'une fonction
static StackSize dfsComputeStack(
    const llvm::Function *F,
    const CallGraph &CG,
    const std::map<const llvm::Function*, StackSize> &LocalStack,
    std::map<const llvm::Function*, VisitState> &State,
    AnalysisResult &Res)
{
    auto itState = State.find(F);
    if (itState != State.end()) {
        if (itState->second == Visiting) {
            // Cycle détecté : on marque tous les noeuds actuellement en "Visiting"
            for (auto &p : State) {
                if (p.second == Visiting) {
                    Res.RecursiveFuncs.insert(p.first);
                }
            }
            // On renvoie au moins la stack locale comme borne inférieure
            auto itLocal = LocalStack.find(F);
            return (itLocal != LocalStack.end()) ? itLocal->second : 0;
        } else if (itState->second == Visited) {
            // Déjà calculé
            auto itTotal = Res.TotalStack.find(F);
            return (itTotal != Res.TotalStack.end()) ? itTotal->second : 0;
        }
    }

    // Marquer comme en cours
    State[F] = Visiting;

    auto itLocal = LocalStack.find(F);
    StackSize local = (itLocal != LocalStack.end()) ? itLocal->second : 0;
    StackSize maxCallee = 0;

    auto itCG = CG.find(F);
    if (itCG != CG.end()) {
        for (const llvm::Function *Callee : itCG->second) {
            StackSize calleeStack = dfsComputeStack(Callee, CG, LocalStack, State, Res);
            if (calleeStack > maxCallee)
                maxCallee = calleeStack;
        }
    }

    StackSize total = local + maxCallee;
    Res.TotalStack[F] = total;
    State[F] = Visited;
    return total;
}

static AnalysisResult computeGlobalStackUsage(
    const CallGraph &CG,
    const std::map<const llvm::Function*, StackSize> &LocalStack)
{
    AnalysisResult Res;
    std::map<const llvm::Function*, VisitState> State;

    // Initialisation explicite (optionnel, NotVisited = 0)
    for (auto &p : LocalStack) {
        State[p.first] = NotVisited;
    }

    for (auto &p : LocalStack) {
        const llvm::Function *F = p.first;
        if (State[F] == NotVisited) {
            dfsComputeStack(F, CG, LocalStack, State, Res);
        }
    }

    return Res;
}

// ============================================================================
//  Détection d’auto-récursion “infinie” (sans base case visible)
//  Heuristique :
//    - on cherche les blocs qui appellent F elle-même
//    - on construit un DominatorTree
//    - si TOUTES les instructions de retour sont dominées par un bloc
//      contenant un self-call, alors tout retour nécessite au moins un
//      appel récursif -> récursion inconditionnelle, risque certain d’overflow
// ============================================================================

static bool detectInfiniteSelfRecursion(llvm::Function &F)
{
    if (F.isDeclaration())
        return false;

    const llvm::Function *Self = &F;

    // Collecte des blocs contenant au moins un appel à soi-même
    std::vector<llvm::BasicBlock*> SelfCallBlocks;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            const llvm::Function *Callee = nullptr;

            if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                Callee = CI->getCalledFunction();
            } else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                Callee = II->getCalledFunction();
            }

            if (Callee == Self) {
                SelfCallBlocks.push_back(&BB);
                break; // un bloc suffit
            }
        }
    }

    if (SelfCallBlocks.empty())
        return false; // pas d’auto-récursion directe

    // Construire le dominator tree
    llvm::DominatorTree DT(F);

    // S’il n’y a aucun return, mais qu’il y a un self-call, on considère ça
    // comme potentielle boucle infinie (noreturn ou UB).
    bool hasReturn = false;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            if (llvm::isa<llvm::ReturnInst>(&I)) {
                hasReturn = true;

                bool dominatedBySelfCall = false;
                for (llvm::BasicBlock *SCB : SelfCallBlocks) {
                    if (DT.dominates(SCB, &BB)) {
                        dominatedBySelfCall = true;
                        break;
                    }
                }

                // On a trouvé un return qui n’est pas forcément précédé
                // d’un self-call -> il existe un chemin qui retourne sans
                // récursion -> base case visible.
                if (!dominatedBySelfCall)
                    return false;
            }
        }
    }

    if (!hasReturn) {
        // Pas de return explicite + self-call => on part du principe que
        // c’est une récursion non bornée (ex: boucle infinie, noreturn, etc.)
        return true;
    }

    // Tous les returns sont dominés par au moins un self-call -> toute sortie
    // de la fonction implique au moins un appel récursif. Sans analyse plus
    // fine des conditions, on considère ça comme récursion potentiellement
    // non bornée.
    return true;
}

// ============================================================================
//  main
// ============================================================================

int main(int argc, char **argv)
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic err;

    if (argc < 2) {
        llvm::errs() << "Usage: stack_usage_analyzer <file.ll>\n";
        return 1;
    }

    std::unique_ptr<llvm::Module> mod = llvm::parseIRFile(argv[1], err, context);
    if (!mod) {
        err.print("stack_usage_analyzer", llvm::errs());
        return 1;
    }

    const llvm::DataLayout &DL = mod->getDataLayout();

    // 1) Stack locale par fonction
    std::map<const llvm::Function*, StackSize> LocalStack;
    for (llvm::Function &F : *mod) {
        if (F.isDeclaration())
            continue;
        StackSize sz = computeLocalStack(F, DL);
        LocalStack[&F] = sz;
    }

    // 2) Graphe d'appels
    CallGraph CG = buildCallGraph(*mod);

    // 3) Propagation + détection de récursivité (cycles)
    AnalysisResult Res = computeGlobalStackUsage(CG, LocalStack);

    // 4) Détection d’auto-récursion “infinie” pour les fonctions récursives
    for (llvm::Function &F : *mod) {
        if (F.isDeclaration())
            continue;
        const llvm::Function *Fn = &F;
        if (!Res.RecursiveFuncs.count(Fn))
            continue;

        if (detectInfiniteSelfRecursion(F)) {
            Res.InfiniteRecursionFuncs.insert(Fn);
        }
    }

    // Limite de stack "thread" (exemple : 8 MiB)
    const StackSize StackLimit = 8ull * 1024ull * 1024ull;

    // 5) Affichage des résultats
    for (llvm::Function &F : *mod) {
        if (F.isDeclaration())
            continue;

        const llvm::Function *Fn = &F;

        StackSize local = 0;
        StackSize total = 0;

        auto itLocal = LocalStack.find(Fn);
        if (itLocal != LocalStack.end())
            local = itLocal->second;

        auto itTotal = Res.TotalStack.find(Fn);
        if (itTotal != Res.TotalStack.end())
            total = itTotal->second;

        llvm::outs() << "Function: " << F.getName() << "\n";
        llvm::outs() << "  local stack: " << local << " bytes\n";
        llvm::outs() << "  max stack (including callees): " << total << " bytes\n";

        if (Res.RecursiveFuncs.count(Fn)) {
            llvm::outs() << "  [!] recursive or mutually recursive function detected\n";
        }

        if (Res.InfiniteRecursionFuncs.count(Fn)) {
            llvm::outs() << "  [!!!] unconditional self recursion detected (no base case)\n";
            llvm::outs() << "       this will eventually overflow the stack at runtime\n";
        }

        if (total > StackLimit) {
            llvm::outs() << "  [!] potential stack overflow: exceeds limit of "
                         << StackLimit << " bytes\n";
        }

        llvm::outs() << "\n";
    }

    return 0;
}
