#include "analysis/StackBufferAnalysis.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "analysis/IntRanges.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        struct RecursionGuard
        {
            llvm::SmallPtrSetImpl<const llvm::Value*>& set;
            const llvm::Value* value;
            RecursionGuard(llvm::SmallPtrSetImpl<const llvm::Value*>& s, const llvm::Value* v)
                : set(s), value(v)
            {
                set.insert(value);
            }
            ~RecursionGuard()
            {
                set.erase(value);
            }
        };

        // Taille (en nombre d'éléments) pour une alloca de tableau sur la stack
        static std::optional<StackSize> getAllocaElementCount(llvm::AllocaInst* AI)
        {
            using namespace llvm;

            Type* elemTy = AI->getAllocatedType();
            StackSize count = 1;

            // Cas "char test[10];" => alloca [10 x i8]
            if (auto* arrTy = dyn_cast<ArrayType>(elemTy))
            {
                count *= arrTy->getNumElements();
                elemTy = arrTy->getElementType();
            }

            // Cas "alloca i8, i64 10" => alloca tableau avec taille constante
            if (AI->isArrayAllocation())
            {
                if (auto* C = dyn_cast<ConstantInt>(AI->getArraySize()))
                {
                    count *= C->getZExtValue();
                }
                else
                {
                    // taille non constante - analyse plus compliquée, on ignore pour l'instant
                    return std::nullopt;
                }
            }

            return count;
        }

        static const llvm::AllocaInst* resolveArrayAllocaFromPointerInternal(
            const llvm::Value* V, llvm::Function& F, std::vector<std::string>& path,
            llvm::SmallPtrSetImpl<const llvm::Value*>& recursionStack, int depth)
        {
            using namespace llvm;

            if (!V)
                return nullptr;
            if (depth > 64)
                return nullptr;
            if (recursionStack.contains(V))
                return nullptr;

            RecursionGuard guard(recursionStack, V);

            auto isArrayAlloca = [](const AllocaInst* AI) -> bool
            {
                Type* T = AI->getAllocatedType();
                // On considère comme "buffer de stack" :
                //  - les vrais tableaux,
                //  - les allocas de type tableau (VLA côté IR),
                //  - les structs qui contiennent au moins un champ tableau.
                if (T->isArrayTy() || AI->isArrayAllocation())
                    return true;

                if (auto* ST = llvm::dyn_cast<llvm::StructType>(T))
                {
                    for (unsigned i = 0; i < ST->getNumElements(); ++i)
                    {
                        if (ST->getElementType(i)->isArrayTy())
                            return true;
                    }
                }
                return false;
            };

            // Pour éviter les boucles d'aliasing bizarres
            SmallPtrSet<const Value*, 16> visited;
            const Value* cur = V;

            while (cur && !visited.contains(cur))
            {
                visited.insert(cur);
                if (cur->hasName())
                    path.push_back(cur->getName().str());

                // Cas 1 : on tombe sur une alloca.
                if (auto* AI = dyn_cast<AllocaInst>(cur))
                {
                    if (isArrayAlloca(AI))
                    {
                        // Alloca d'un buffer de stack (tableau) : cible finale.
                        return AI;
                    }

                    // Sinon, c'est très probablement une variable locale de type pointeur
                    // (char *ptr; char **pp; etc.). On parcourt les stores vers cette
                    // variable pour voir quelles valeurs lui sont assignées, et on
                    // tente de remonter jusqu'à une vraie alloca de tableau.
                    const AllocaInst* foundAI = nullptr;

                    for (BasicBlock& BB : F)
                    {
                        for (Instruction& I : BB)
                        {
                            auto* SI = dyn_cast<StoreInst>(&I);
                            if (!SI)
                                continue;
                            if (SI->getPointerOperand() != AI)
                                continue;

                            const Value* storedPtr = SI->getValueOperand();
                            std::vector<std::string> subPath;
                            const AllocaInst* cand = resolveArrayAllocaFromPointerInternal(
                                storedPtr, F, subPath, recursionStack, depth + 1);
                            if (!cand)
                                continue;

                            if (!foundAI)
                            {
                                foundAI = cand;
                                // Append subPath to path
                                path.insert(path.end(), subPath.begin(), subPath.end());
                            }
                            else if (foundAI != cand)
                            {
                                // Plusieurs bases différentes : aliasing ambigu,
                                // on préfère abandonner plutôt que de se tromper.
                                return nullptr;
                            }
                        }
                    }
                    return foundAI;
                }

                // Cas 2 : bitcast -> on remonte l'opérande.
                if (auto* BC = dyn_cast<BitCastInst>(cur))
                {
                    cur = BC->getOperand(0);
                    continue;
                }

                // Cas 3 : GEP -> on remonte sur le pointeur de base.
                if (auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                {
                    cur = GEP->getPointerOperand();
                    continue;
                }

                // Cas 4 : load d'un pointeur. Exemple typique :
                //    char *ptr = test;
                //    char *p2  = ptr;
                //    char **pp = &ptr;
                //    (*pp)[i] = ...
                //
                // On remonte au "container" du pointeur (variable locale, ou autre valeur)
                // en suivant l'opérande du load.
                if (auto* LI = dyn_cast<LoadInst>(cur))
                {
                    cur = LI->getPointerOperand();
                    continue;
                }

                // Cas 5 : PHI de pointeurs (fusion de plusieurs alias) :
                // on tente de résoudre chaque incoming et on s'assure qu'ils
                // pointent tous vers la même alloca de tableau.
                if (auto* PN = dyn_cast<PHINode>(cur))
                {
                    const AllocaInst* foundAI = nullptr;
                    std::vector<std::string> phiPath;
                    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                    {
                        const Value* inV = PN->getIncomingValue(i);
                        std::vector<std::string> subPath;
                        const AllocaInst* cand = resolveArrayAllocaFromPointerInternal(
                            inV, F, subPath, recursionStack, depth + 1);
                        if (!cand)
                            continue;
                        if (!foundAI)
                        {
                            foundAI = cand;
                            phiPath = subPath;
                        }
                        else if (foundAI != cand)
                        {
                            // PHI mélange plusieurs bases différentes : trop ambigu.
                            return nullptr;
                        }
                    }
                    path.insert(path.end(), phiPath.begin(), phiPath.end());
                    return foundAI;
                }

                // Autres cas (arguments, globales complexes, etc.) : on arrête l'heuristique.
                break;
            }

            return nullptr;
        }

        static const llvm::AllocaInst* resolveArrayAllocaFromPointer(const llvm::Value* V,
                                                                     llvm::Function& F,
                                                                     std::vector<std::string>& path)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> recursionStack;
            return resolveArrayAllocaFromPointerInternal(V, F, path, recursionStack, 0);
        }

        static void
        analyzeStackBufferOverflowsInFunction(llvm::Function& F,
                                              std::vector<StackBufferOverflowIssue>& out)
        {
            using namespace llvm;

            auto ranges = computeIntRangesFromICmps(F);

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* GEP = dyn_cast<GetElementPtrInst>(&I);
                    if (!GEP)
                        continue;

                    // 1) Trouver la base du pointeur (test, &test[0], ptr, etc.)
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> aliasPath;
                    const AllocaInst* AI = resolveArrayAllocaFromPointer(basePtr, F, aliasPath);
                    if (!AI)
                        continue;

                    // 2) Déterminer la taille logique du tableau ciblé et récupérer l'index
                    //    On essaie d'abord de la déduire du type traversé par la GEP
                    //    (cas struct S { char buf[10]; }; s.buf[i]) puis on retombe
                    //    sur la taille de l'alloca pour les cas plus simples (char buf[10]).
                    StackSize arraySize = 0;
                    Value* idxVal = nullptr;

                    Type* srcElemTy = GEP->getSourceElementType();

                    if (auto* arrTy = dyn_cast<ArrayType>(srcElemTy))
                    {
                        // Cas direct : alloca [N x T]; GEP indices [0, i]
                        if (GEP->getNumIndices() < 2)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        ++idxIt; // saute le premier indice (souvent 0)
                        idxVal = idxIt->get();
                        arraySize = arrTy->getNumElements();
                    }
                    else if (auto* ST = dyn_cast<StructType>(srcElemTy))
                    {
                        // Cas struct avec champ tableau:
                        //   %ptr = getelementptr inbounds %struct.S, %struct.S* %s,
                        //          i32 0, i32 <field>, i64 %i
                        //
                        // On attend donc au moins 3 indices: [0, field, i]
                        if (GEP->getNumIndices() >= 3)
                        {
                            auto idxIt = GEP->idx_begin();

                            // premier indice (souvent 0)
                            auto* idx0 = dyn_cast<ConstantInt>(idxIt->get());
                            ++idxIt;
                            // second indice: index de champ dans la struct
                            auto* fieldIdxC = dyn_cast<ConstantInt>(idxIt->get());
                            ++idxIt;

                            if (idx0 && fieldIdxC)
                            {
                                unsigned fieldIdx =
                                    static_cast<unsigned>(fieldIdxC->getZExtValue());
                                if (fieldIdx < ST->getNumElements())
                                {
                                    Type* fieldTy = ST->getElementType(fieldIdx);
                                    if (auto* fieldArrTy = dyn_cast<ArrayType>(fieldTy))
                                    {
                                        arraySize = fieldArrTy->getNumElements();
                                        // Troisième indice = index dans le tableau interne
                                        idxVal = idxIt->get();
                                    }
                                }
                            }
                        }
                    }

                    // Si on n'a pas réussi à déduire une taille via la GEP,
                    // on retombe sur la taille dérivée de l'alloca (cas char buf[10]; ptr = buf; ptr[i]).
                    if (arraySize == 0 || !idxVal)
                    {
                        auto maybeCount = getAllocaElementCount(const_cast<AllocaInst*>(AI));
                        if (!maybeCount)
                            continue;
                        arraySize = *maybeCount;
                        if (arraySize == 0)
                            continue;

                        // Pour ces cas-là, on considère le premier indice comme l'index logique.
                        if (GEP->getNumIndices() < 1)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        idxVal = idxIt->get();
                    }

                    std::string varName =
                        AI->hasName() ? AI->getName().str() : std::string("<unnamed>");

                    // "baseIdxVal" = variable de boucle "i" sans les casts (sext/zext...)
                    Value* baseIdxVal = idxVal;
                    while (auto* cast = dyn_cast<CastInst>(baseIdxVal))
                    {
                        baseIdxVal = cast->getOperand(0);
                    }

                    // 4) Cas index constant : test[11]
                    if (auto* CIdx = dyn_cast<ConstantInt>(idxVal))
                    {
                        auto idxValue = CIdx->getSExtValue();
                        if (idxValue < 0 || static_cast<StackSize>(idxValue) >= arraySize)
                        {
                            for (User* GU : GEP->users())
                            {
                                if (auto* S = dyn_cast<StoreInst>(GU))
                                {
                                    StackBufferOverflowIssue report;
                                    report.funcName = F.getName().str();
                                    report.varName = varName;
                                    report.arraySize = arraySize;
                                    report.indexOrUpperBound = static_cast<StackSize>(idxValue);
                                    report.isWrite = true;
                                    report.indexIsConstant = true;
                                    report.inst = S;
                                    report.aliasPathVec = aliasPath;
                                    if (!aliasPath.empty())
                                    {
                                        std::reverse(aliasPath.begin(), aliasPath.end());
                                        std::string chain;
                                        for (size_t i = 0; i < aliasPath.size(); ++i)
                                        {
                                            chain += aliasPath[i];
                                            if (i + 1 < aliasPath.size())
                                                chain += " -> ";
                                        }
                                        report.aliasPath = chain;
                                    }
                                    out.push_back(std::move(report));
                                }
                                else if (auto* L = dyn_cast<LoadInst>(GU))
                                {
                                    StackBufferOverflowIssue report;
                                    report.funcName = F.getName().str();
                                    report.varName = varName;
                                    report.arraySize = arraySize;
                                    report.indexOrUpperBound = static_cast<StackSize>(idxValue);
                                    report.isWrite = false;
                                    report.indexIsConstant = true;
                                    report.inst = L;
                                    report.aliasPathVec = aliasPath;
                                    if (!aliasPath.empty())
                                    {
                                        std::reverse(aliasPath.begin(), aliasPath.end());
                                        std::string chain;
                                        for (size_t i = 0; i < aliasPath.size(); ++i)
                                        {
                                            chain += aliasPath[i];
                                            if (i + 1 < aliasPath.size())
                                                chain += " -> ";
                                        }
                                        report.aliasPath = chain;
                                    }
                                    out.push_back(std::move(report));
                                }
                            }
                        }
                        continue;
                    }

                    // 5) Cas index variable : test[i] / ptr[i]
                    // On regarde si on a un intervalle pour la valeur de base (i, pas le cast)
                    const Value* key = baseIdxVal;

                    // Si l'index vient d'un load (pattern -O0 : load i, icmp, load i, gep),
                    // on utilise le pointeur sous-jacent comme clé (l'alloca de i).
                    if (auto* LI = dyn_cast<LoadInst>(baseIdxVal))
                    {
                        key = LI->getPointerOperand();
                    }

                    auto itRange = ranges.find(key);
                    if (itRange == ranges.end())
                    {
                        // pas de borne connue => on ne dit rien ici
                        continue;
                    }

                    const IntRange& R = itRange->second;

                    // 5.a) Borne supérieure hors bornes: UB >= arraySize
                    if (R.hasUpper && R.upper >= 0 && static_cast<StackSize>(R.upper) >= arraySize)
                    {
                        StackSize ub = static_cast<StackSize>(R.upper);

                        for (User* GU : GEP->users())
                        {
                            if (auto* S = dyn_cast<StoreInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.inst = S;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.inst = L;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                        }
                    }

                    // 5.b) Borne inférieure négative: LB < 0  => index potentiellement négatif
                    if (R.hasLower && R.lower < 0)
                    {
                        for (User* GU : GEP->users())
                        {
                            if (auto* S = dyn_cast<StoreInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.inst = S;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.inst = L;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                        }
                    }
                    // Si R.hasUpper && R.upper < arraySize et (pas de LB problématique),
                    // on considère l'accès comme probablement sûr.
                }
            }
        }

        static void analyzeMultipleStoresInFunction(llvm::Function& F,
                                                    std::vector<MultipleStoreIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            struct Info
            {
                std::size_t storeCount = 0;
                llvm::SmallPtrSet<const Value*, 8> indexKeys;
                const AllocaInst* AI = nullptr;
            };

            std::map<const AllocaInst*, Info> infoMap;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* S = dyn_cast<StoreInst>(&I);
                    if (!S)
                        continue;

                    Value* ptr = S->getPointerOperand();
                    auto* GEP = dyn_cast<GetElementPtrInst>(ptr);
                    if (!GEP)
                        continue;

                    // On remonte à la base pour trouver une alloca de tableau sur la stack.
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> dummyAliasPath;
                    const AllocaInst* AI =
                        resolveArrayAllocaFromPointer(basePtr, F, dummyAliasPath);
                    if (!AI)
                        continue;

                    // On récupère l'expression d'index utilisée dans le GEP.
                    Value* idxVal = nullptr;
                    Type* srcElemTy = GEP->getSourceElementType();

                    if (auto* arrTy = dyn_cast<ArrayType>(srcElemTy))
                    {
                        // Pattern [N x T]* -> indices [0, i]
                        if (GEP->getNumIndices() < 2)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        ++idxIt; // saute le premier indice (souvent 0)
                        idxVal = idxIt->get();
                    }
                    else
                    {
                        // Pattern T* -> indice unique [i] (cas char *ptr = test; ptr[i])
                        if (GEP->getNumIndices() < 1)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        idxVal = idxIt->get();
                    }

                    if (!idxVal)
                        continue;

                    // On normalise un peu la clé d'index en enlevant les casts SSA.
                    const Value* idxKey = idxVal;
                    while (auto* cast = dyn_cast<CastInst>(const_cast<Value*>(idxKey)))
                    {
                        idxKey = cast->getOperand(0);
                    }

                    auto& info = infoMap[AI];
                    info.AI = AI;
                    info.storeCount++;
                    info.indexKeys.insert(idxKey);
                }
            }

            // Construction des warnings pour chaque buffer qui reçoit plusieurs stores.
            for (auto& entry : infoMap)
            {
                const AllocaInst* AI = entry.first;
                const Info& info = entry.second;

                if (info.storeCount <= 1)
                    continue; // un seul store -> pas de warning

                MultipleStoreIssue issue;
                issue.funcName = F.getName().str();
                issue.varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                issue.storeCount = info.storeCount;
                issue.distinctIndexCount = info.indexKeys.size();
                issue.allocaInst = AI;

                out.push_back(std::move(issue));
            }
        }
    } // namespace

    std::vector<StackBufferOverflowIssue>
    analyzeStackBufferOverflows(llvm::Module& mod,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<StackBufferOverflowIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackBufferOverflowsInFunction(F, out);
        }

        return out;
    }

    std::vector<MultipleStoreIssue>
    analyzeMultipleStores(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<MultipleStoreIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeMultipleStoresInFunction(F, out);
        }

        return out;
    }
} // namespace ctrace::stack::analysis
