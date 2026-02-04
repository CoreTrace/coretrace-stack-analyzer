#include "StackUsageAnalyzer.hpp"

#include <cstdint>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <cstdio> // std::snprintf

#include <optional>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/CFG.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>

#include "compilerlib/compiler.h"
#include "mangle.hpp"

namespace ctrace::stack
{

    // ============================================================================
    // Types internes
    // ============================================================================

    using CallGraph = std::map<const llvm::Function*, std::vector<const llvm::Function*>>;

    enum VisitState
    {
        NotVisited = 0,
        Visiting = 1,
        Visited = 2
    };

    struct StackEstimate
    {
        StackSize bytes = 0;
        bool unknown = false;
    };

    struct LocalStackInfo
    {
        StackSize bytes = 0;
        bool unknown = false;
        bool hasDynamicAlloca = false;
        std::vector<std::pair<std::string, StackSize>> localAllocas;
    };

    // État interne pour la propagation
    struct InternalAnalysisState
    {
        std::map<const llvm::Function*, StackEstimate> TotalStack; // stack max, callees inclus
        std::set<const llvm::Function*> RecursiveFuncs;         // fonctions dans au moins un cycle
        std::set<const llvm::Function*> InfiniteRecursionFuncs; // auto-récursion “infinie”
    };

    // Rapport interne pour les dépassements de buffer sur la stack
    struct StackBufferOverflow
    {
        std::string funcName;
        std::string varName;
        StackSize arraySize = 0;
        StackSize indexOrUpperBound = 0; // utilisé pour les bornes sup (UB) ou index constant
        bool isWrite = false;
        bool indexIsConstant = false;
        const llvm::Instruction* inst = nullptr;

        // Nouveau : violation basée sur une borne inférieure (index potentiellement négatif)
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

    // Intervalle d'entier pour une valeur : borne inférieure / supérieure (signées)
    struct IntRange
    {
        bool hasLower = false;
        long long lower = 0;
        bool hasUpper = false;
        long long upper = 0;
    };

    // Rapport interne pour les allocations dynamiques sur la stack (VLA / alloca variable)
    struct DynamicAllocaIssue
    {
        std::string funcName;
        std::string varName;
        std::string typeName;
        const llvm::AllocaInst* allocaInst = nullptr;
    };

    // Internal report for alloca/VLA usages with potentially risky sizes
    struct AllocaUsageIssue
    {
        std::string funcName;
        std::string varName;
        const llvm::AllocaInst* allocaInst = nullptr;

        bool userControlled = false;      // size derived from argument / non-local value
        bool sizeIsConst = false;         // size known exactly
        bool hasUpperBound = false;       // bounded size (from ICmp-derived range)
        bool isRecursive = false;         // function participates in a recursion cycle
        bool isInfiniteRecursive = false; // unconditional self recursion

        StackSize sizeBytes = 0;       // exact size in bytes (if sizeIsConst)
        StackSize upperBoundBytes = 0; // upper bound in bytes (if hasUpperBound)
    };

    // Rapport interne pour les usages dangereux de memcpy/memset sur la stack
    struct MemIntrinsicIssue
    {
        std::string funcName;
        std::string varName;
        std::string intrinsicName; // "memcpy" / "memset" / "memmove"
        StackSize destSizeBytes = 0;
        StackSize lengthBytes = 0;
        const llvm::Instruction* inst = nullptr;
    };

    // Rapport interne pour plusieurs stores dans un même buffer de stack
    struct MultipleStoreIssue
    {
        std::string funcName;
        std::string varName;
        std::size_t storeCount = 0;         // nombre total de StoreInst vers ce buffer
        std::size_t distinctIndexCount = 0; // nombre d'expressions d'index distinctes
        const llvm::AllocaInst* allocaInst = nullptr;
    };

    // Rapport interne pour les fuites de pointeurs vers la stack
    struct StackPointerEscapeIssue
    {
        std::string funcName;
        std::string varName;
        std::string
            escapeKind; // "return", "store_global", "store_unknown", "call_arg", "call_callback"
        std::string targetName; // nom du global, si applicable
        const llvm::Instruction* inst = nullptr;
    };

    // Rapport interne pour les reconstructions invalides de pointeur de base (offsetof/container_of)
    struct InvalidBaseReconstructionIssue
    {
        std::string funcName;
        std::string varName;        // nom de la variable alloca (stack object)
        std::string sourceMember;   // membre source (ex: "b")
        int64_t offsetUsed = 0;     // offset utilisé dans le calcul (peut être négatif)
        std::string targetType;     // type vers lequel on cast (ex: "struct A*")
        bool isOutOfBounds = false; // true si on peut prouver que c'est hors bornes
        const llvm::Instruction* inst = nullptr;
    };

    // Rapport interne pour les paramètres qui peuvent être rendus const (pointee/referent)
    struct ConstParamIssue
    {
        std::string funcName;
        std::string paramName;
        std::string currentType;
        std::string suggestedType;
        std::string suggestedTypeAlt;
        bool pointerConstOnly = false; // ex: T * const param
        bool isReference = false;
        bool isRvalueRef = false;
        unsigned line = 0;
        unsigned column = 0;
    };
    // Analyse intra-fonction pour détecter les "fuites" de pointeurs de stack :
    //  - retour d'une adresse de variable locale (return buf;)
    //  - stockage de l'adresse d'une variable locale dans un global (global = buf;)
    //
    // Heuristique : pour chaque AllocaInst, on remonte son graphe d'utilisation
    // en suivant les bitcast, GEP, PHI, select de type pointeur, et on marque
    // comme "escape" :
    //   - tout return qui renvoie une valeur dérivée de cette alloca
    //   - tout store qui écrit cette valeur dans une GlobalVariable.
    static void analyzeStackPointerEscapesInFunction(llvm::Function& F,
                                                     std::vector<StackPointerEscapeIssue>& out)
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

                // On limite l'analyse aux slots "classiques" de stack (tout alloca)
                SmallPtrSet<const Value*, 16> visited;
                SmallVector<const Value*, 8> worklist;
                worklist.push_back(AI);

                while (!worklist.empty())
                {
                    const Value* V = worklist.back();
                    worklist.pop_back();
                    if (visited.contains(V))
                        continue;
                    visited.insert(V);

                    for (const Use& U : V->uses())
                    {
                        const User* Usr = U.getUser();

                        // 1) Retour direct ou via chaîne d'alias : return <V>
                        if (auto* RI = dyn_cast<ReturnInst>(Usr))
                        {
                            StackPointerEscapeIssue issue;
                            issue.funcName = F.getName().str();
                            issue.varName =
                                AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                            issue.escapeKind = "return";
                            issue.targetName = {};
                            issue.inst = RI;
                            out.push_back(std::move(issue));
                            continue;
                        }

                        // 2) Stockage de l'adresse : global = <V>; ou *out = <V>;
                        if (auto* SI = dyn_cast<StoreInst>(Usr))
                        {
                            // Si la valeur stockée est notre pointeur (ou un alias de celui-ci)
                            if (SI->getValueOperand() == V)
                            {
                                const Value* dstRaw = SI->getPointerOperand();
                                const Value* dst = dstRaw->stripPointerCasts();

                                // 2.a) Stockage direct dans une variable globale : fuite évidente
                                if (auto* GV = dyn_cast<GlobalVariable>(dst))
                                {
                                    StackPointerEscapeIssue issue;
                                    issue.funcName = F.getName().str();
                                    issue.varName = AI->hasName() ? AI->getName().str()
                                                                  : std::string("<unnamed>");
                                    issue.escapeKind = "store_global";
                                    issue.targetName =
                                        GV->hasName() ? GV->getName().str() : std::string{};
                                    issue.inst = SI;
                                    out.push_back(std::move(issue));
                                    continue;
                                }

                                // 2.b) Stockage via un pointeur non local (ex: *out = buf;)
                                // On ne connaît pas la durée de vie de la mémoire pointée par dst,
                                // mais si ce n'est pas une alloca de cette fonction, on considère
                                // que le pointeur de stack peut s'échapper (paramètre, heap, etc.).
                                if (!isa<AllocaInst>(dst))
                                {
                                    StackPointerEscapeIssue issue;
                                    issue.funcName = F.getName().str();
                                    issue.varName = AI->hasName() ? AI->getName().str()
                                                                  : std::string("<unnamed>");
                                    issue.escapeKind = "store_unknown";
                                    issue.targetName =
                                        dst->hasName() ? dst->getName().str() : std::string{};
                                    issue.inst = SI;
                                    out.push_back(std::move(issue));
                                    continue;
                                }

                                // 2.c) Stockage dans une alloca locale : on laisse l'alias
                                // continuer à être exploré via la boucle de travail. On ne
                                // considère pas cela comme une fuite immédiate.
                                const AllocaInst* dstAI = cast<AllocaInst>(dst);
                                worklist.push_back(dstAI);
                            }
                            // Sinon, c'est un store vers la stack ou un autre emplacement local
                            // qui ne contient pas directement notre pointeur, pas une fuite en soi.
                            continue;
                        }

                        // 3) Passage de l'adresse à un appel de fonction : cb(buf); ou f(buf);
                        if (auto* CB = dyn_cast<CallBase>(Usr))
                        {
                            // On inspecte tous les arguments; si l'un d'eux est V (ou un alias direct),
                            // on considère que l'adresse de la variable locale est transmise.
                            for (unsigned argIndex = 0; argIndex < CB->arg_size(); ++argIndex)
                            {
                                if (CB->getArgOperand(argIndex) != V)
                                    continue;

                                const Value* calledVal = CB->getCalledOperand();
                                const Value* calledStripped =
                                    calledVal ? calledVal->stripPointerCasts() : nullptr;
                                const Function* directCallee =
                                    calledStripped ? dyn_cast<Function>(calledStripped) : nullptr;
                                if (CB->paramHasAttr(argIndex, llvm::Attribute::NoCapture) ||
                                    CB->paramHasAttr(argIndex, llvm::Attribute::ByVal) ||
                                    CB->paramHasAttr(argIndex, llvm::Attribute::ByRef))
                                {
                                    continue;
                                }
                                if (directCallee)
                                {
                                    llvm::StringRef calleeName = directCallee->getName();
                                    if (calleeName.contains("unique_ptr") ||
                                        calleeName.contains("make_unique"))
                                    {
                                        continue;
                                    }
                                }

                                StackPointerEscapeIssue issue;
                                issue.funcName = F.getName().str();
                                issue.varName =
                                    AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                                issue.inst = cast<Instruction>(CB);

                                if (!directCallee)
                                {
                                    // Appel indirect via pointeur de fonction : callback typique.
                                    issue.escapeKind = "call_callback";
                                    issue.targetName.clear();
                                }
                                else
                                {
                                    // Appel direct : on n'a pas de connaissance précise de la sémantique
                                    // de la fonction appelée; on marque ça comme une fuite potentielle
                                    // plus permissive.
                                    issue.escapeKind = "call_arg";
                                    issue.targetName = directCallee->hasName()
                                                           ? directCallee->getName().str()
                                                           : std::string{};
                                }

                                out.push_back(std::move(issue));
                            }

                            // On ne propage pas l'alias via l'appel, mais on considère que
                            // l'adresse peut être capturée par la fonction appelée.
                            continue;
                        }

                        // 4) Propagation des alias de pointeurs :
                        if (auto* BC = dyn_cast<BitCastInst>(Usr))
                        {
                            if (BC->getType()->isPointerTy())
                                worklist.push_back(BC);
                            continue;
                        }
                        if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                        {
                            worklist.push_back(GEP);
                            continue;
                        }
                        if (auto* PN = dyn_cast<PHINode>(Usr))
                        {
                            if (PN->getType()->isPointerTy())
                                worklist.push_back(PN);
                            continue;
                        }
                        if (auto* Sel = dyn_cast<SelectInst>(Usr))
                        {
                            if (Sel->getType()->isPointerTy())
                                worklist.push_back(Sel);
                            continue;
                        }

                        // Autres usages (load, comparaison, etc.) : pas une fuite,
                        // et on ne propage pas davantage.
                    }
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // Détection des reconstructions invalides de pointeur de base (offsetof/container_of)
    // --------------------------------------------------------------------------

    // Forward declaration
    static std::optional<StackSize> getAllocaTotalSizeBytes(const llvm::AllocaInst* AI,
                                                            const llvm::DataLayout& DL);
    static const llvm::Value* getPtrToIntOperand(const llvm::Value* V);

    // Pointer origin (base alloca + offset from base)
    struct PtrOrigin
    {
        const llvm::AllocaInst* alloca = nullptr;
        int64_t offset = 0;
    };

    static bool recordVisitedOffset(std::map<const llvm::Value*, std::set<int64_t>>& visited,
                                    const llvm::Value* V, int64_t offset)
    {
        auto& setRef = visited[V];
        return setRef.insert(offset).second;
    }

    static bool getGEPConstantOffsetAndBase(const llvm::Value* V, const llvm::DataLayout& DL,
                                            int64_t& outOffset, const llvm::Value*& outBase)
    {
        using namespace llvm;

        if (auto* GEP = dyn_cast<GetElementPtrInst>(V))
        {
            APInt offset(64, 0);
            if (!GEP->accumulateConstantOffset(DL, offset))
                return false;
            outOffset = offset.getSExtValue();
            outBase = GEP->getPointerOperand();
            return true;
        }

        if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            if (CE->getOpcode() == Instruction::GetElementPtr)
            {
                auto* GEP = cast<GEPOperator>(CE);
                APInt offset(64, 0);
                if (!GEP->accumulateConstantOffset(DL, offset))
                    return false;
                outOffset = offset.getSExtValue();
                outBase = GEP->getPointerOperand();
                return true;
            }
        }

        return false;
    }

    static const llvm::Value* stripIntCasts(const llvm::Value* V)
    {
        using namespace llvm;

        const Value* Cur = V;
        while (Cur)
        {
            if (auto* CI = dyn_cast<CastInst>(Cur))
            {
                const Value* Op = CI->getOperand(0);
                if (CI->getType()->isIntegerTy() && Op->getType()->isIntegerTy())
                {
                    Cur = Op;
                    continue;
                }
            }
            else if (auto* CE = dyn_cast<ConstantExpr>(Cur))
            {
                if (CE->isCast())
                {
                    const Value* Op = CE->getOperand(0);
                    if (CE->getType()->isIntegerTy() && Op->getType()->isIntegerTy())
                    {
                        Cur = Op;
                        continue;
                    }
                }
            }
            break;
        }
        return Cur;
    }

    static bool isLoadFromAlloca(const llvm::Value* V, const llvm::AllocaInst* AI)
    {
        using namespace llvm;
        if (auto* LI = dyn_cast<LoadInst>(V))
        {
            const Value* PtrOp = LI->getPointerOperand()->stripPointerCasts();
            return PtrOp == AI;
        }
        return false;
    }

    static bool valueDependsOnAlloca(const llvm::Value* V, const llvm::AllocaInst* AI,
                                     llvm::SmallPtrSet<const llvm::Value*, 32>& visited)
    {
        using namespace llvm;

        if (!V)
            return false;
        if (!visited.insert(V).second)
            return false;

        if (isLoadFromAlloca(V, AI))
            return true;

        if (auto* I = dyn_cast<Instruction>(V))
        {
            for (const Value* Op : I->operands())
            {
                if (valueDependsOnAlloca(Op, AI, visited))
                    return true;
            }
        }
        else if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            for (const Value* Op : CE->operands())
            {
                if (valueDependsOnAlloca(Op, AI, visited))
                    return true;
            }
        }

        return false;
    }

    static bool matchAllocaLoadAddSub(const llvm::Value* V, const llvm::AllocaInst* AI,
                                      int64_t& deltaOut)
    {
        using namespace llvm;

        const Value* lhs = nullptr;
        const Value* rhs = nullptr;
        unsigned opcode = 0;

        if (auto* BO = dyn_cast<BinaryOperator>(V))
        {
            opcode = BO->getOpcode();
            lhs = BO->getOperand(0);
            rhs = BO->getOperand(1);
        }
        else if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            opcode = CE->getOpcode();
            lhs = CE->getOperand(0);
            rhs = CE->getOperand(1);
        }
        else
        {
            return false;
        }

        if (opcode != Instruction::Add && opcode != Instruction::Sub)
            return false;

        const auto* lhsC = dyn_cast<ConstantInt>(lhs);
        const auto* rhsC = dyn_cast<ConstantInt>(rhs);
        bool lhsIsLoad = isLoadFromAlloca(lhs, AI);
        bool rhsIsLoad = isLoadFromAlloca(rhs, AI);

        if (opcode == Instruction::Add)
        {
            if (lhsIsLoad && rhsC)
            {
                deltaOut = rhsC->getSExtValue();
                return true;
            }
            if (rhsIsLoad && lhsC)
            {
                deltaOut = lhsC->getSExtValue();
                return true;
            }
        }
        else if (opcode == Instruction::Sub)
        {
            if (lhsIsLoad && rhsC)
            {
                deltaOut = -rhsC->getSExtValue();
                return true;
            }
            // Do not accept C - load
        }

        return false;
    }

    struct PtrIntMatch
    {
        const llvm::Value* ptrOperand = nullptr;
        int64_t offset = 0;
        bool sawOffset = false;
    };

    static void collectPtrToIntMatches(const llvm::Value* V,
                                       llvm::SmallVectorImpl<PtrIntMatch>& out)
    {
        using namespace llvm;

        struct IntWorkItem
        {
            const Value* val = nullptr;
            int64_t offset = 0;
            bool sawOffset = false;
        };

        SmallVector<IntWorkItem, 16> worklist;
        std::map<const Value*, std::map<int64_t, unsigned>> visited;

        auto recordVisited = [&](const Value* Val, int64_t offset, bool sawOffset)
        {
            unsigned bit = sawOffset ? 2u : 1u;
            unsigned& flags = visited[Val][offset];
            if (flags & bit)
                return false;
            flags |= bit;
            return true;
        };

        worklist.push_back({V, 0, false});
        recordVisited(V, 0, false);

        while (!worklist.empty())
        {
            const Value* Cur = stripIntCasts(worklist.back().val);
            int64_t curOffset = worklist.back().offset;
            bool curSawOffset = worklist.back().sawOffset;
            worklist.pop_back();

            if (const Value* PtrOp = getPtrToIntOperand(Cur))
            {
                out.push_back({PtrOp, curOffset, curSawOffset});
                continue;
            }

            const Value* lhs = nullptr;
            const Value* rhs = nullptr;
            unsigned opcode = 0;

            if (auto* BO = dyn_cast<BinaryOperator>(Cur))
            {
                opcode = BO->getOpcode();
                lhs = BO->getOperand(0);
                rhs = BO->getOperand(1);
            }
            else if (auto* CE = dyn_cast<ConstantExpr>(Cur))
            {
                opcode = CE->getOpcode();
                lhs = CE->getOperand(0);
                rhs = CE->getOperand(1);
            }

            if (opcode == Instruction::Add || opcode == Instruction::Sub)
            {
                const auto* lhsC = dyn_cast<ConstantInt>(lhs);
                const auto* rhsC = dyn_cast<ConstantInt>(rhs);
                if (rhsC)
                {
                    int64_t delta = rhsC->getSExtValue();
                    if (opcode == Instruction::Sub)
                        delta = -delta;
                    int64_t newOffset = curOffset + delta;
                    if (recordVisited(lhs, newOffset, true))
                        worklist.push_back({lhs, newOffset, true});
                    continue;
                }
                if (lhsC && opcode == Instruction::Add)
                {
                    int64_t delta = lhsC->getSExtValue();
                    int64_t newOffset = curOffset + delta;
                    if (recordVisited(rhs, newOffset, true))
                        worklist.push_back({rhs, newOffset, true});
                    continue;
                }
                // sub with constant on LHS is not a valid reconstruction
            }

            if (auto* PN = dyn_cast<PHINode>(Cur))
            {
                for (const Value* In : PN->incoming_values())
                {
                    if (recordVisited(In, curOffset, curSawOffset))
                        worklist.push_back({In, curOffset, curSawOffset});
                }
                continue;
            }
            if (auto* Sel = dyn_cast<SelectInst>(Cur))
            {
                const Value* T = Sel->getTrueValue();
                const Value* F = Sel->getFalseValue();
                if (recordVisited(T, curOffset, curSawOffset))
                    worklist.push_back({T, curOffset, curSawOffset});
                if (recordVisited(F, curOffset, curSawOffset))
                    worklist.push_back({F, curOffset, curSawOffset});
                continue;
            }

            if (auto* LI = dyn_cast<LoadInst>(Cur))
            {
                const Value* PtrOp = LI->getPointerOperand()->stripPointerCasts();
                if (auto* AI = dyn_cast<AllocaInst>(PtrOp))
                {
                    Type* allocTy = AI->getAllocatedType();
                    if (allocTy && allocTy->isIntegerTy())
                    {
                        SmallVector<const Value*, 8> seeds;
                        SmallVector<int64_t, 8> deltas;

                        for (const User* Usr : AI->users())
                        {
                            auto* SI = dyn_cast<StoreInst>(Usr);
                            if (!SI)
                                continue;
                            if (SI->getPointerOperand()->stripPointerCasts() != AI)
                                continue;
                            const Value* StoredVal = SI->getValueOperand();

                            int64_t delta = 0;
                            if (matchAllocaLoadAddSub(StoredVal, AI, delta))
                            {
                                deltas.push_back(delta);
                                continue;
                            }

                            llvm::SmallPtrSet<const Value*, 32> depVisited;
                            if (!valueDependsOnAlloca(StoredVal, AI, depVisited))
                            {
                                seeds.push_back(StoredVal);
                            }
                        }

                        if (!seeds.empty())
                        {
                            for (const Value* Seed : seeds)
                            {
                                if (recordVisited(Seed, curOffset, curSawOffset))
                                    worklist.push_back({Seed, curOffset, curSawOffset});
                                for (int64_t delta : deltas)
                                {
                                    int64_t newOffset = curOffset + delta;
                                    if (recordVisited(Seed, newOffset, true))
                                        worklist.push_back({Seed, newOffset, true});
                                }
                            }
                        }
                        else
                        {
                            // Fallback: explore stored values directly (may be imprecise).
                            for (const User* Usr : AI->users())
                            {
                                auto* SI = dyn_cast<StoreInst>(Usr);
                                if (!SI)
                                    continue;
                                if (SI->getPointerOperand()->stripPointerCasts() != AI)
                                    continue;
                                const Value* StoredVal = SI->getValueOperand();
                                if (recordVisited(StoredVal, curOffset, curSawOffset))
                                    worklist.push_back({StoredVal, curOffset, curSawOffset});
                            }
                        }
                        continue;
                    }
                }
            }
        }
    }

    static void collectPointerOrigins(const llvm::Value* V, const llvm::DataLayout& DL,
                                      llvm::SmallVectorImpl<PtrOrigin>& out)
    {
        using namespace llvm;

        SmallVector<std::pair<const Value*, int64_t>, 16> worklist;
        std::map<const Value*, std::set<int64_t>> visited;

        worklist.push_back({V, 0});
        recordVisitedOffset(visited, V, 0);

        while (!worklist.empty())
        {
            const Value* Cur = worklist.back().first;
            int64_t currentOffset = worklist.back().second;
            worklist.pop_back();

            if (auto* AI = dyn_cast<AllocaInst>(Cur))
            {
                Type* allocaTy = AI->getAllocatedType();
                if (allocaTy->isPointerTy())
                {
                    // Pointer slot: follow what gets stored there.
                    for (const User* Usr : AI->users())
                    {
                        if (auto* SI = dyn_cast<StoreInst>(Usr))
                        {
                            if (SI->getPointerOperand() != AI)
                                continue;
                            const Value* StoredVal = SI->getValueOperand();
                            if (recordVisitedOffset(visited, StoredVal, currentOffset))
                            {
                                worklist.push_back({StoredVal, currentOffset});
                            }
                        }
                    }
                    continue;
                }

                out.push_back({AI, currentOffset});
                continue;
            }

            if (auto* BC = dyn_cast<BitCastInst>(Cur))
            {
                const Value* Src = BC->getOperand(0);
                if (recordVisitedOffset(visited, Src, currentOffset))
                    worklist.push_back({Src, currentOffset});
                continue;
            }

            if (auto* ASC = dyn_cast<AddrSpaceCastInst>(Cur))
            {
                const Value* Src = ASC->getOperand(0);
                if (recordVisitedOffset(visited, Src, currentOffset))
                    worklist.push_back({Src, currentOffset});
                continue;
            }

            int64_t gepOffset = 0;
            const Value* gepBase = nullptr;
            if (getGEPConstantOffsetAndBase(Cur, DL, gepOffset, gepBase))
            {
                int64_t newOffset = currentOffset + gepOffset;
                if (recordVisitedOffset(visited, gepBase, newOffset))
                    worklist.push_back({gepBase, newOffset});
                continue;
            }

            if (auto* LI = dyn_cast<LoadInst>(Cur))
            {
                const Value* PtrOp = LI->getPointerOperand();
                if (recordVisitedOffset(visited, PtrOp, currentOffset))
                    worklist.push_back({PtrOp, currentOffset});
                continue;
            }

            if (auto* PN = dyn_cast<PHINode>(Cur))
            {
                for (const Value* In : PN->incoming_values())
                {
                    if (recordVisitedOffset(visited, In, currentOffset))
                        worklist.push_back({In, currentOffset});
                }
                continue;
            }

            if (auto* Sel = dyn_cast<SelectInst>(Cur))
            {
                const Value* T = Sel->getTrueValue();
                const Value* F = Sel->getFalseValue();
                if (recordVisitedOffset(visited, T, currentOffset))
                    worklist.push_back({T, currentOffset});
                if (recordVisitedOffset(visited, F, currentOffset))
                    worklist.push_back({F, currentOffset});
                continue;
            }

            if (auto* CE = dyn_cast<ConstantExpr>(Cur))
            {
                if (CE->getOpcode() == Instruction::BitCast ||
                    CE->getOpcode() == Instruction::AddrSpaceCast)
                {
                    const Value* Src = CE->getOperand(0);
                    if (recordVisitedOffset(visited, Src, currentOffset))
                        worklist.push_back({Src, currentOffset});
                }
            }
        }
    }

    static bool isPointerDereferencedOrUsed(const llvm::Value* V)
    {
        using namespace llvm;

        SmallVector<const Value*, 16> worklist;
        SmallPtrSet<const Value*, 32> visited;
        worklist.push_back(V);

        while (!worklist.empty())
        {
            const Value* Cur = worklist.back();
            worklist.pop_back();
            if (!visited.insert(Cur).second)
                continue;

            for (const Use& U : Cur->uses())
            {
                const User* Usr = U.getUser();

                if (auto* LI = dyn_cast<LoadInst>(Usr))
                {
                    if (LI->getPointerOperand() == Cur)
                        return true;
                    continue;
                }
                if (auto* SI = dyn_cast<StoreInst>(Usr))
                {
                    if (SI->getPointerOperand() == Cur)
                        return true;
                    if (SI->getValueOperand() == Cur)
                    {
                        const Value* dst = SI->getPointerOperand()->stripPointerCasts();
                        if (auto* AI = dyn_cast<AllocaInst>(dst))
                        {
                            Type* allocTy = AI->getAllocatedType();
                            if (allocTy && allocTy->isPointerTy())
                            {
                                for (const User* AUser : AI->users())
                                {
                                    if (auto* LI = dyn_cast<LoadInst>(AUser))
                                    {
                                        if (LI->getPointerOperand()->stripPointerCasts() == AI)
                                        {
                                            worklist.push_back(LI);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    continue;
                }
                if (auto* RMW = dyn_cast<AtomicRMWInst>(Usr))
                {
                    if (RMW->getPointerOperand() == Cur)
                        return true;
                    continue;
                }
                if (auto* CX = dyn_cast<AtomicCmpXchgInst>(Usr))
                {
                    if (CX->getPointerOperand() == Cur)
                        return true;
                    continue;
                }
                if (auto* MI = dyn_cast<MemIntrinsic>(Usr))
                {
                    if (MI->getRawDest() == Cur)
                        return true;
                    if (auto* MTI = dyn_cast<MemTransferInst>(MI))
                    {
                        if (MTI->getRawSource() == Cur)
                            return true;
                    }
                    continue;
                }

                if (auto* BC = dyn_cast<BitCastInst>(Usr))
                {
                    worklist.push_back(BC);
                    continue;
                }
                if (auto* ASC = dyn_cast<AddrSpaceCastInst>(Usr))
                {
                    worklist.push_back(ASC);
                    continue;
                }
                if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                {
                    worklist.push_back(GEP);
                    continue;
                }
                if (auto* PN = dyn_cast<PHINode>(Usr))
                {
                    worklist.push_back(PN);
                    continue;
                }
                if (auto* Sel = dyn_cast<SelectInst>(Usr))
                {
                    worklist.push_back(Sel);
                    continue;
                }
                if (auto* CE = dyn_cast<ConstantExpr>(Usr))
                {
                    worklist.push_back(CE);
                    continue;
                }
            }
        }

        return false;
    }

    static const llvm::Value* getPtrToIntOperand(const llvm::Value* V)
    {
        using namespace llvm;

        if (auto* PTI = dyn_cast<PtrToIntInst>(V))
            return PTI->getOperand(0);
        if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            if (CE->getOpcode() == Instruction::PtrToInt)
                return CE->getOperand(0);
        }
        return nullptr;
    }

    static bool matchPtrToIntAddSub(const llvm::Value* V, const llvm::Value*& outPtrOperand,
                                    int64_t& outOffset)
    {
        using namespace llvm;

        const Value* lhs = nullptr;
        const Value* rhs = nullptr;
        unsigned opcode = 0;

        if (auto* BO = dyn_cast<BinaryOperator>(V))
        {
            opcode = BO->getOpcode();
            lhs = BO->getOperand(0);
            rhs = BO->getOperand(1);
        }
        else if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            opcode = CE->getOpcode();
            lhs = CE->getOperand(0);
            rhs = CE->getOperand(1);
        }
        else
        {
            return false;
        }

        if (opcode != Instruction::Add && opcode != Instruction::Sub)
            return false;

        const Value* lhsPtr = getPtrToIntOperand(lhs);
        const Value* rhsPtr = getPtrToIntOperand(rhs);

        const ConstantInt* lhsC = dyn_cast<ConstantInt>(lhs);
        const ConstantInt* rhsC = dyn_cast<ConstantInt>(rhs);

        if (lhsPtr && rhsC)
        {
            outPtrOperand = lhsPtr;
            outOffset = rhsC->getSExtValue();
            if (opcode == Instruction::Sub)
                outOffset = -outOffset;
            return true;
        }

        if (rhsPtr && lhsC)
        {
            if (opcode == Instruction::Sub)
            {
                // Pattern is C - ptrtoint(P): not a base reconstruction.
                return false;
            }
            outPtrOperand = rhsPtr;
            outOffset = lhsC->getSExtValue();
            return true;
        }

        return false;
    }

    // Détecte les patterns de type:
    //   inttoptr(ptrtoint(P) +/- C)
    //   ou (char*)P +/- C
    // où P est un membre d'une structure sur la stack et C est un offset constant,
    // et le résultat est utilisé comme pointeur vers la structure de base.
    //
    // Ce pattern est typiquement utilisé dans container_of() / offsetof() mais peut
    // être incorrect si l'offset ne correspond pas au membre réel.
    static void
    analyzeInvalidBaseReconstructionsInFunction(llvm::Function& F, const llvm::DataLayout& DL,
                                                std::vector<InvalidBaseReconstructionIssue>& out)
    {
        using namespace llvm;

        if (F.isDeclaration())
            return;

        // Recherche des allocas (objets stack)
        std::map<const AllocaInst*, std::pair<std::string, uint64_t>> allocaInfo;

        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {
                auto* AI = dyn_cast<AllocaInst>(&I);
                if (!AI)
                    continue;

                // Calcul de la taille de l'objet alloué
                std::optional<StackSize> sizeOpt = getAllocaTotalSizeBytes(AI, DL);
                if (!sizeOpt.has_value())
                    continue; // Taille dynamique, on ne peut pas analyser

                std::string varName =
                    AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                allocaInfo[AI] = {varName, sizeOpt.value()};
            }
        }

        // Maintenant, recherchons les patterns de reconstruction de pointeur de base
        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {

                // Pattern 1: inttoptr(ptrtoint(P) +/- C)
                if (auto* ITP = dyn_cast<IntToPtrInst>(&I))
                {
                    if (!isPointerDereferencedOrUsed(ITP))
                        continue;

                    Value* IntVal = ITP->getOperand(0);

                    SmallVector<PtrIntMatch, 8> matches;
                    collectPtrToIntMatches(IntVal, matches);
                    if (matches.empty())
                        continue;

                    struct AggEntry
                    {
                        std::set<int64_t> memberOffsets;
                        bool anyOutOfBounds = false;
                        bool anyNonZeroResult = false;
                        std::string varName;
                        uint64_t allocaSize = 0;
                        std::string targetType;
                    };

                    std::map<std::pair<const llvm::AllocaInst*, int64_t>, AggEntry> agg;

                    for (const auto& match : matches)
                    {
                        if (!match.sawOffset)
                            continue;

                        SmallVector<PtrOrigin, 8> origins;
                        collectPointerOrigins(match.ptrOperand, DL, origins);
                        if (origins.empty())
                            continue;

                        for (const auto& origin : origins)
                        {
                            auto it = allocaInfo.find(origin.alloca);
                            if (it == allocaInfo.end())
                                continue;

                            const std::string& varName = it->second.first;
                            uint64_t allocaSize = it->second.second;

                            int64_t resultOffset = origin.offset + match.offset;
                            bool isOutOfBounds =
                                (resultOffset < 0) ||
                                (static_cast<uint64_t>(resultOffset) >= allocaSize);

                            std::string targetType;
                            Type* targetTy = ITP->getType();
                            if (auto* PtrTy = dyn_cast<PointerType>(targetTy))
                            {
                                raw_string_ostream rso(targetType);
                                PtrTy->print(rso);
                            }

                            auto key = std::make_pair(origin.alloca, match.offset);
                            auto& entry = agg[key];
                            entry.memberOffsets.insert(origin.offset);
                            entry.anyOutOfBounds |= isOutOfBounds;
                            if (resultOffset != 0)
                                entry.anyNonZeroResult = true;
                            entry.varName = varName;
                            entry.allocaSize = allocaSize;
                            entry.targetType = targetType.empty() ? "<unknown>" : targetType;
                        }
                    }

                    for (auto& kv : agg)
                    {
                        const auto& entry = kv.second;
                        if (entry.memberOffsets.empty())
                            continue;
                        if (!entry.anyOutOfBounds && !entry.anyNonZeroResult)
                            continue;

                        std::ostringstream memberStr;
                        if (entry.memberOffsets.size() == 1)
                        {
                            int64_t mo = *entry.memberOffsets.begin();
                            memberStr << (mo != 0 ? "offset +" + std::to_string(mo) : "base");
                        }
                        else
                        {
                            memberStr << "offsets ";
                            bool first = true;
                            for (int64_t mo : entry.memberOffsets)
                            {
                                if (!first)
                                    memberStr << ", ";
                                memberStr << (mo != 0 ? "+" + std::to_string(mo) : "base");
                                first = false;
                            }
                        }

                        InvalidBaseReconstructionIssue issue;
                        issue.funcName = F.getName().str();
                        issue.varName = entry.varName;
                        issue.sourceMember = memberStr.str();
                        issue.offsetUsed = kv.first.second;
                        issue.targetType = entry.targetType;
                        issue.isOutOfBounds = entry.anyOutOfBounds;
                        issue.inst = &I;

                        out.push_back(std::move(issue));
                    }
                }

                // Pattern 2: GEP avec offset constant sur un membre
                // (équivalent à (char*)ptr + offset)
                if (auto* GEP = dyn_cast<GetElementPtrInst>(&I))
                {
                    if (!isPointerDereferencedOrUsed(GEP))
                        continue;

                    int64_t gepOffset = 0;
                    const Value* PtrOp = nullptr;
                    if (!getGEPConstantOffsetAndBase(GEP, DL, gepOffset, PtrOp))
                        continue;

                    SmallVector<PtrOrigin, 8> origins;
                    collectPointerOrigins(PtrOp, DL, origins);
                    if (origins.empty())
                        continue;

                    struct AggEntry
                    {
                        std::set<int64_t> memberOffsets;
                        bool anyOutOfBounds = false;
                        bool anyNonZeroResult = false;
                        std::string varName;
                        std::string targetType;
                    };

                    std::map<const llvm::AllocaInst*, AggEntry> agg;

                    for (const auto& origin : origins)
                    {
                        if (origin.offset == 0 && gepOffset >= 0)
                        {
                            // Likely a normal member access from the base object.
                            continue;
                        }

                        auto it = allocaInfo.find(origin.alloca);
                        if (it == allocaInfo.end())
                            continue;

                        const std::string& varName = it->second.first;
                        uint64_t allocaSize = it->second.second;

                        int64_t resultOffset = origin.offset + gepOffset;
                        bool isOutOfBounds = (resultOffset < 0) ||
                                             (static_cast<uint64_t>(resultOffset) >= allocaSize);

                        std::string targetType;
                        Type* targetTy = GEP->getType();
                        raw_string_ostream rso(targetType);
                        targetTy->print(rso);

                        auto& entry = agg[origin.alloca];
                        entry.memberOffsets.insert(origin.offset);
                        entry.anyOutOfBounds |= isOutOfBounds;
                        if (resultOffset != 0)
                            entry.anyNonZeroResult = true;
                        entry.varName = varName;
                        entry.targetType = targetType;
                    }

                    for (auto& kv : agg)
                    {
                        const auto& entry = kv.second;
                        if (entry.memberOffsets.empty())
                            continue;
                        if (!entry.anyOutOfBounds && !entry.anyNonZeroResult)
                            continue;

                        std::ostringstream memberStr;
                        if (entry.memberOffsets.size() == 1)
                        {
                            int64_t mo = *entry.memberOffsets.begin();
                            memberStr << (mo != 0 ? "offset +" + std::to_string(mo) : "base");
                        }
                        else
                        {
                            memberStr << "offsets ";
                            bool first = true;
                            for (int64_t mo : entry.memberOffsets)
                            {
                                if (!first)
                                    memberStr << ", ";
                                memberStr << (mo != 0 ? "+" + std::to_string(mo) : "base");
                                first = false;
                            }
                        }

                        InvalidBaseReconstructionIssue issue;
                        issue.funcName = F.getName().str();
                        issue.varName = entry.varName;
                        issue.sourceMember = memberStr.str();
                        issue.offsetUsed = gepOffset;
                        issue.targetType = entry.targetType;
                        issue.isOutOfBounds = entry.anyOutOfBounds;
                        issue.inst = &I;

                        out.push_back(std::move(issue));
                    }
                }
            }
        }
    }

    // --------------------------------------------------------------------------
    // Helpers pour analyser les allocas et les bornes d'index
    // --------------------------------------------------------------------------

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

    // Taille totale en octets pour une alloca sur la stack.
    // Retourne std::nullopt si la taille dépend d'une valeur non constante (VLA).
    static std::optional<StackSize> getAllocaTotalSizeBytes(const llvm::AllocaInst* AI,
                                                            const llvm::DataLayout& DL)
    {
        using namespace llvm;

        Type* allocatedTy = AI->getAllocatedType();

        // Cas alloca [N x T] (taille connue dans le type)
        if (!AI->isArrayAllocation())
        {
            return DL.getTypeAllocSize(allocatedTy);
        }

        // Cas alloca T, i64 <N> (taille passée séparément)
        if (auto* C = dyn_cast<ConstantInt>(AI->getArraySize()))
        {
            uint64_t count = C->getZExtValue();
            uint64_t elemSize = DL.getTypeAllocSize(allocatedTy);
            return count * elemSize;
        }

        // Taille dynamique - traitée par l'analyse DynamicAllocaIssue
        return std::nullopt;
    }

    // Analyse des comparaisons ICmp pour déduire les intervalles d'entiers (bornes inf/sup)
    static std::map<const llvm::Value*, IntRange> computeIntRangesFromICmps(llvm::Function& F)
    {
        using namespace llvm;

        std::map<const Value*, IntRange> ranges;

        auto applyConstraint =
            [&ranges](const Value* V, bool hasLB, long long newLB, bool hasUB, long long newUB)
        {
            auto& R = ranges[V];
            if (hasLB)
            {
                if (!R.hasLower || newLB > R.lower)
                {
                    R.hasLower = true;
                    R.lower = newLB;
                }
            }
            if (hasUB)
            {
                if (!R.hasUpper || newUB < R.upper)
                {
                    R.hasUpper = true;
                    R.upper = newUB;
                }
            }
        };

        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {
                auto* icmp = dyn_cast<ICmpInst>(&I);
                if (!icmp)
                    continue;

                Value* op0 = icmp->getOperand(0);
                Value* op1 = icmp->getOperand(1);

                ConstantInt* C = nullptr;
                Value* V = nullptr;

                // On cherche un pattern "V ? C" ou "C ? V"
                if ((C = dyn_cast<ConstantInt>(op1)) && !isa<ConstantInt>(op0))
                {
                    V = op0;
                }
                else if ((C = dyn_cast<ConstantInt>(op0)) && !isa<ConstantInt>(op1))
                {
                    V = op1;
                }
                else
                {
                    continue;
                }

                auto pred = icmp->getPredicate();

                bool hasLB = false, hasUB = false;
                long long lb = 0, ub = 0;

                auto updateForSigned = [&](bool valueIsOp0)
                {
                    long long c = C->getSExtValue();
                    if (valueIsOp0)
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_SLT: // V < C  => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_SLE: // V <= C => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_SGT: // V > C  => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_SGE: // V >= C => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ: // V == C => [C, C]
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            // approximation : V != C  => V <= C (très conservateur)
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        // C ? V  <=>  V ? C (inversé)
                        switch (pred)
                        {
                        case ICmpInst::ICMP_SGT: // C > V  => V < C => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_SGE: // C >= V => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_SLT: // C < V  => V > C => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_SLE: // C <= V => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ: // C == V => [C, C]
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                };

                auto updateForUnsigned = [&](bool valueIsOp0)
                {
                    unsigned long long cu = C->getZExtValue();
                    long long c = static_cast<long long>(cu);
                    if (valueIsOp0)
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_ULT: // V < C  => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_ULE: // V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_UGT: // V > C  => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_UGE: // V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ:
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_UGT: // C > V => V < C
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_UGE: // C >= V => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_ULT: // C < V => V > C
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_ULE: // C <= V => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ:
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                };

                bool valueIsOp0 = (V == op0);

                // On choisit le groupe de prédicats
                if (pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_SLE ||
                    pred == ICmpInst::ICMP_SGT || pred == ICmpInst::ICMP_SGE ||
                    pred == ICmpInst::ICMP_EQ || pred == ICmpInst::ICMP_NE)
                {
                    updateForSigned(valueIsOp0);
                }
                else if (pred == ICmpInst::ICMP_ULT || pred == ICmpInst::ICMP_ULE ||
                         pred == ICmpInst::ICMP_UGT || pred == ICmpInst::ICMP_UGE)
                {
                    updateForUnsigned(valueIsOp0);
                }

                if (!(hasLB || hasUB))
                    continue;

                // Applique la contrainte sur V lui-même
                applyConstraint(V, hasLB, lb, hasUB, ub);

                // Et éventuellement sur le pointeur sous-jacent si V est un load
                if (auto* LI = dyn_cast<LoadInst>(V))
                {
                    const Value* ptr = LI->getPointerOperand();
                    applyConstraint(ptr, hasLB, lb, hasUB, ub);
                }
            }
        }

        return ranges;
    }

    // Heuristic: determine if a Value is user-controlled
    // (function argument, load from a non-local pointer, call result, etc.).
    static bool isValueUserControlledImpl(const llvm::Value* V, const llvm::Function& F,
                                          llvm::SmallPtrSet<const llvm::Value*, 16>& visited,
                                          int depth = 0)
    {
        using namespace llvm;

        if (!V || depth > 20)
            return false;
        if (visited.contains(V))
            return false;
        visited.insert(V);

        if (isa<Argument>(V))
            return true; // function argument -> considered user-provided

        if (isa<Constant>(V))
            return false;

        if (auto* LI = dyn_cast<LoadInst>(V))
        {
            const Value* ptr = LI->getPointerOperand()->stripPointerCasts();
            if (isa<Argument>(ptr))
                return true; // load through pointer passed as argument
            if (!isa<AllocaInst>(ptr))
            {
                return true; // load from non-local memory (global / heap / unknown)
            }
            // If it's a local alloca, inspect what gets stored there.
            const AllocaInst* AI = cast<AllocaInst>(ptr);
            for (const Use& U : AI->uses())
            {
                if (auto* SI = dyn_cast<StoreInst>(U.getUser()))
                {
                    if (SI->getPointerOperand()->stripPointerCasts() != ptr)
                        continue;
                    if (isValueUserControlledImpl(SI->getValueOperand(), F, visited, depth + 1))
                        return true;
                }
            }
        }

        if (auto* CB = dyn_cast<CallBase>(V))
        {
            // Value produced by a call: conservatively treat as external/user input.
            (void)F;
            (void)CB;
            return true;
        }

        if (auto* I = dyn_cast<Instruction>(V))
        {
            for (const Value* Op : I->operands())
            {
                if (isValueUserControlledImpl(Op, F, visited, depth + 1))
                    return true;
            }
        }
        else if (auto* CE = dyn_cast<ConstantExpr>(V))
        {
            for (const Value* Op : CE->operands())
            {
                if (isValueUserControlledImpl(Op, F, visited, depth + 1))
                    return true;
            }
        }

        return false;
    }

    static bool isValueUserControlled(const llvm::Value* V, const llvm::Function& F)
    {
        llvm::SmallPtrSet<const llvm::Value*, 16> visited;
        return isValueUserControlledImpl(V, F, visited, 0);
    }

    // Try to recover a human-friendly name for an alloca even when the instruction
    // itself is unnamed (typical IR for "char *buf = alloca(n);").
    static std::string deriveAllocaName(const llvm::AllocaInst* AI)
    {
        using namespace llvm;

        if (!AI)
            return std::string("<unnamed>");
        if (AI->hasName())
            return AI->getName().str();

        SmallPtrSet<const Value*, 16> visited;
        SmallVector<const Value*, 8> worklist;
        worklist.push_back(AI);

        while (!worklist.empty())
        {
            const Value* V = worklist.back();
            worklist.pop_back();
            if (!visited.insert(V).second)
                continue;

            for (const Use& U : V->uses())
            {
                const User* Usr = U.getUser();

                if (auto* DVI = dyn_cast<DbgValueInst>(Usr))
                {
                    if (auto* var = DVI->getVariable())
                    {
                        if (!var->getName().empty())
                            return var->getName().str();
                    }
                    continue;
                }

                if (auto* SI = dyn_cast<StoreInst>(Usr))
                {
                    if (SI->getValueOperand() != V)
                        continue;
                    const Value* dst = SI->getPointerOperand()->stripPointerCasts();
                    if (auto* dstAI = dyn_cast<AllocaInst>(dst))
                    {
                        if (dstAI->hasName())
                            return dstAI->getName().str();
                    }
                    worklist.push_back(dst);
                    continue;
                }

                if (auto* BC = dyn_cast<BitCastInst>(Usr))
                {
                    worklist.push_back(BC);
                    continue;
                }
                if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                {
                    worklist.push_back(GEP);
                    continue;
                }
                if (auto* PN = dyn_cast<PHINode>(Usr))
                {
                    if (PN->getType()->isPointerTy())
                        worklist.push_back(PN);
                    continue;
                }
                if (auto* Sel = dyn_cast<SelectInst>(Usr))
                {
                    if (Sel->getType()->isPointerTy())
                        worklist.push_back(Sel);
                    continue;
                }
            }
        }

        return std::string("<unnamed>");
    }

    static std::string formatFunctionNameForMessage(const std::string& name)
    {
        if (ctrace_tools::isMangled(name))
            return ctrace_tools::demangle(name.c_str());
        return name;
    }

    struct TypeQualifiers
    {
        bool isConst = false;
        bool isVolatile = false;
        bool isRestrict = false;
    };

    struct StrippedDIType
    {
        const llvm::DIType* type = nullptr;
        TypeQualifiers quals;
    };

    struct ParamDebugInfo
    {
        std::string name;
        const llvm::DIType* type = nullptr;
        unsigned line = 0;
        unsigned column = 0;
    };

    struct ParamTypeInfo
    {
        const llvm::DIType* originalType = nullptr;
        const llvm::DIType* pointeeType = nullptr;        // unqualified, typedefs stripped
        const llvm::DIType* pointeeDisplayType = nullptr; // unqualified, typedefs preserved
        bool isPointer = false;
        bool isReference = false;
        bool isRvalueReference = false;
        bool pointerConst = false;
        bool pointerVolatile = false;
        bool pointerRestrict = false;
        bool pointeeConst = false;
        bool pointeeVolatile = false;
        bool pointeeRestrict = false;
        bool isDoublePointer = false;
        bool isVoid = false;
        bool isFunctionPointer = false;
    };

    static const llvm::DIType* stripTypedefs(const llvm::DIType* type)
    {
        using namespace llvm;
        const DIType* cur = type;
        while (cur)
        {
            auto* DT = dyn_cast<DIDerivedType>(cur);
            if (!DT)
                break;
            auto tag = DT->getTag();
            if (tag == dwarf::DW_TAG_typedef)
            {
                cur = DT->getBaseType();
                continue;
            }
            break;
        }
        return cur;
    }

    static StrippedDIType stripQualifiers(const llvm::DIType* type)
    {
        using namespace llvm;
        StrippedDIType out;
        out.type = type;

        while (out.type)
        {
            auto* DT = dyn_cast<DIDerivedType>(out.type);
            if (!DT)
                break;
            auto tag = DT->getTag();
            if (tag == dwarf::DW_TAG_const_type)
            {
                out.quals.isConst = true;
                out.type = DT->getBaseType();
                continue;
            }
            if (tag == dwarf::DW_TAG_volatile_type)
            {
                out.quals.isVolatile = true;
                out.type = DT->getBaseType();
                continue;
            }
            if (tag == dwarf::DW_TAG_restrict_type)
            {
                out.quals.isRestrict = true;
                out.type = DT->getBaseType();
                continue;
            }
            break;
        }

        return out;
    }

    static std::string formatDITypeName(const llvm::DIType* type)
    {
        using namespace llvm;
        if (!type)
            return std::string("<unknown type>");

        if (auto* BT = dyn_cast<DIBasicType>(type))
        {
            if (!BT->getName().empty())
                return BT->getName().str();
        }

        if (auto* CT = dyn_cast<DICompositeType>(type))
        {
            if (!CT->getName().empty())
                return CT->getName().str();
            if (!CT->getIdentifier().empty())
                return CT->getIdentifier().str();
        }

        if (auto* DT = dyn_cast<DIDerivedType>(type))
        {
            auto tag = DT->getTag();
            if (tag == dwarf::DW_TAG_typedef && !DT->getName().empty())
            {
                return DT->getName().str();
            }
            if ((tag == dwarf::DW_TAG_const_type) || (tag == dwarf::DW_TAG_volatile_type) ||
                (tag == dwarf::DW_TAG_restrict_type))
            {
                return formatDITypeName(DT->getBaseType());
            }
            if (!DT->getName().empty())
                return DT->getName().str();
        }

        if (auto* ST = dyn_cast<DISubroutineType>(type))
        {
            (void)ST;
            return std::string("<function>");
        }

        return std::string("<anonymous type>");
    }

    static bool buildParamTypeInfo(const llvm::DIType* type, ParamTypeInfo& info)
    {
        using namespace llvm;
        if (!type)
            return false;

        info.originalType = type;

        StrippedDIType top = stripQualifiers(type);
        info.pointerConst = top.quals.isConst;
        info.pointerVolatile = top.quals.isVolatile;
        info.pointerRestrict = top.quals.isRestrict;

        const DIType* topType = stripTypedefs(top.type);
        auto* derived = dyn_cast<DIDerivedType>(topType);
        if (!derived)
            return false;

        auto tag = derived->getTag();
        if (tag == dwarf::DW_TAG_pointer_type)
        {
            info.isPointer = true;
        }
        else if (tag == dwarf::DW_TAG_reference_type)
        {
            info.isReference = true;
        }
        else if (tag == dwarf::DW_TAG_rvalue_reference_type)
        {
            info.isReference = true;
            info.isRvalueReference = true;
        }
        else
        {
            return false;
        }

        const DIType* baseType = derived->getBaseType();
        StrippedDIType base = stripQualifiers(baseType);
        info.pointeeConst = base.quals.isConst;
        info.pointeeVolatile = base.quals.isVolatile;
        info.pointeeRestrict = base.quals.isRestrict;
        info.pointeeDisplayType = base.type ? base.type : baseType;

        const DIType* baseNoTypedef = stripTypedefs(base.type);
        info.pointeeType = baseNoTypedef;

        if (!baseNoTypedef)
            return true;

        if (auto* baseDerived = dyn_cast<DIDerivedType>(baseNoTypedef))
        {
            auto baseTag = baseDerived->getTag();
            if (baseTag == dwarf::DW_TAG_pointer_type || baseTag == dwarf::DW_TAG_reference_type ||
                baseTag == dwarf::DW_TAG_rvalue_reference_type)
            {
                info.isDoublePointer = true;
            }
        }

        if (isa<DISubroutineType>(baseNoTypedef))
            info.isFunctionPointer = true;

        if (auto* basic = dyn_cast<DIBasicType>(baseNoTypedef))
        {
            if (basic->getName() == "void")
                info.isVoid = true;
        }

        return true;
    }

    static std::string buildTypeString(const ParamTypeInfo& info, const std::string& baseName,
                                       bool addPointeeConst, bool includePointerConst,
                                       const std::string& paramName)
    {
        std::string out;
        if (info.pointeeConst || addPointeeConst)
            out += "const ";
        if (info.pointeeVolatile)
            out += "volatile ";
        out += baseName.empty() ? std::string("<unknown type>") : baseName;

        if (info.isReference)
        {
            out += info.isRvalueReference ? " &&" : " &";
            if (!paramName.empty())
            {
                out += paramName;
            }
            return out;
        }

        if (info.isPointer)
        {
            out += " *";
            if (includePointerConst && info.pointerConst)
                out += " const";
            if (info.pointerVolatile)
                out += " volatile";
            if (info.pointerRestrict)
                out += " restrict";
        }

        if (!paramName.empty())
        {
            if (!out.empty() && (out.back() == '*' || out.back() == '&'))
                out += paramName;
            else
                out += " " + paramName;
        }

        return out;
    }

    static std::string buildPointeeQualPrefix(const ParamTypeInfo& info, bool addConst)
    {
        std::string out;
        if (addConst)
            out += "const ";
        if (info.pointeeVolatile)
            out += "volatile ";
        if (info.pointeeRestrict)
            out += "restrict ";
        return out;
    }

    static ParamDebugInfo getParamDebugInfo(const llvm::Function& F, const llvm::Argument& Arg)
    {
        using namespace llvm;
        ParamDebugInfo info;
        info.name = Arg.getName().str();

        if (auto* SP = F.getSubprogram())
        {
            for (DINode* node : SP->getRetainedNodes())
            {
                auto* var = dyn_cast<DILocalVariable>(node);
                if (!var || !var->isParameter())
                    continue;
                if (var->getArg() != Arg.getArgNo() + 1)
                    continue;
                if (!var->getName().empty())
                    info.name = var->getName().str();
                info.type = var->getType();
                if (var->getLine() != 0)
                    info.line = var->getLine();
                break;
            }

            if (!info.type)
            {
                if (auto* subTy = SP->getType())
                {
                    auto types = subTy->getTypeArray();
                    if (types.size() > Arg.getArgNo() + 1)
                        info.type = types[Arg.getArgNo() + 1];
                }
            }

            if (info.line == 0)
                info.line = SP->getLine();
        }

        return info;
    }

    static bool calleeParamIsReadOnly(const llvm::Function* callee, unsigned argIndex)
    {
        if (!callee || argIndex >= callee->arg_size())
            return false;

        const llvm::Argument& param = *callee->getArg(argIndex);
        ParamDebugInfo dbg = getParamDebugInfo(*callee, param);
        if (!dbg.type)
            return false;

        ParamTypeInfo typeInfo;
        if (!buildParamTypeInfo(dbg.type, typeInfo))
            return false;

        if (typeInfo.isDoublePointer || typeInfo.isVoid || typeInfo.isFunctionPointer)
            return false;

        if (!typeInfo.isPointer && !typeInfo.isReference)
            return false;

        return typeInfo.pointeeConst;
    }

    static bool callArgMayWriteThrough(const llvm::CallBase& CB, unsigned argIndex)
    {
        using namespace llvm;

        const Function* callee = CB.getCalledFunction();
        if (!callee)
        {
            const Value* called = CB.getCalledOperand();
            if (called)
                called = called->stripPointerCasts();
            callee = dyn_cast<Function>(called);
        }

        if (!callee)
            return true;

        if (auto* MI = dyn_cast<MemIntrinsic>(&CB))
        {
            if (isa<MemSetInst>(MI))
                return argIndex == 0;
            if (isa<MemTransferInst>(MI))
                return argIndex == 0;
        }

        if (callee->isIntrinsic())
        {
            switch (callee->getIntrinsicID())
            {
            case Intrinsic::dbg_declare:
            case Intrinsic::dbg_value:
            case Intrinsic::dbg_label:
            case Intrinsic::lifetime_start:
            case Intrinsic::lifetime_end:
            case Intrinsic::invariant_start:
            case Intrinsic::invariant_end:
            case Intrinsic::assume:
                return false;
            default:
                break;
            }
        }

        if (callee->doesNotAccessMemory())
            return false;
        if (callee->onlyReadsMemory())
            return false;

        if (argIndex >= callee->arg_size())
            return true; // varargs or unknown

        const AttributeList& attrs = callee->getAttributes();
        if (attrs.hasParamAttr(argIndex, Attribute::ReadOnly) ||
            attrs.hasParamAttr(argIndex, Attribute::ReadNone))
        {
            return false;
        }
        if (attrs.hasParamAttr(argIndex, Attribute::WriteOnly))
            return true;

        if (calleeParamIsReadOnly(callee, argIndex))
            return false;

        return true;
    }

    static bool valueMayBeWrittenThrough(const llvm::Value* root, const llvm::Function& F)
    {
        using namespace llvm;
        (void)F;

        SmallPtrSet<const Value*, 32> visited;
        SmallVector<const Value*, 16> worklist;
        worklist.push_back(root);

        while (!worklist.empty())
        {
            const Value* V = worklist.pop_back_val();
            if (!visited.insert(V).second)
                continue;

            for (const Use& U : V->uses())
            {
                const User* Usr = U.getUser();

                if (auto* SI = dyn_cast<StoreInst>(Usr))
                {
                    if (SI->getPointerOperand() == V)
                        return true;
                    if (SI->getValueOperand() == V)
                    {
                        const Value* dst = SI->getPointerOperand()->stripPointerCasts();
                        if (auto* AI = dyn_cast<AllocaInst>(dst))
                        {
                            for (const Use& AU : AI->uses())
                            {
                                if (auto* LI = dyn_cast<LoadInst>(AU.getUser()))
                                {
                                    if (LI->getPointerOperand()->stripPointerCasts() == AI)
                                        worklist.push_back(LI);
                                }
                            }
                        }
                        else
                        {
                            return true; // pointer escapes to non-local memory
                        }
                    }
                    continue;
                }

                if (auto* AI = dyn_cast<AtomicRMWInst>(Usr))
                {
                    if (AI->getPointerOperand() == V)
                        return true;
                    continue;
                }

                if (auto* CX = dyn_cast<AtomicCmpXchgInst>(Usr))
                {
                    if (CX->getPointerOperand() == V)
                        return true;
                    continue;
                }

                if (auto* CB = dyn_cast<CallBase>(Usr))
                {
                    for (unsigned i = 0; i < CB->arg_size(); ++i)
                    {
                        if (CB->getArgOperand(i) == V)
                        {
                            if (callArgMayWriteThrough(*CB, i))
                                return true;
                        }
                    }
                    continue;
                }

                if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                {
                    worklist.push_back(GEP);
                    continue;
                }
                if (auto* BC = dyn_cast<BitCastInst>(Usr))
                {
                    worklist.push_back(BC);
                    continue;
                }
                if (auto* ASC = dyn_cast<AddrSpaceCastInst>(Usr))
                {
                    worklist.push_back(ASC);
                    continue;
                }
                if (auto* PN = dyn_cast<PHINode>(Usr))
                {
                    if (PN->getType()->isPointerTy())
                        worklist.push_back(PN);
                    continue;
                }
                if (auto* Sel = dyn_cast<SelectInst>(Usr))
                {
                    if (Sel->getType()->isPointerTy())
                        worklist.push_back(Sel);
                    continue;
                }
                if (auto* CI = dyn_cast<CastInst>(Usr))
                {
                    if (CI->getType()->isPointerTy())
                        worklist.push_back(CI);
                    continue;
                }
                if (isa<PtrToIntInst>(Usr))
                    return true; // unknown aliasing, be conservative
            }
        }

        return false;
    }

    static void analyzeConstParamsInFunction(llvm::Function& F, std::vector<ConstParamIssue>& out)
    {
        using namespace llvm;

        if (F.isDeclaration())
            return;

        for (Argument& Arg : F.args())
        {
            ParamDebugInfo dbg = getParamDebugInfo(F, Arg);
            if (!dbg.type)
                continue;

            ParamTypeInfo typeInfo;
            if (!buildParamTypeInfo(dbg.type, typeInfo))
                continue;

            if (!typeInfo.isPointer && !typeInfo.isReference)
                continue;
            if (typeInfo.isDoublePointer || typeInfo.isVoid || typeInfo.isFunctionPointer)
                continue;
            if (typeInfo.pointeeConst)
                continue;

            if (valueMayBeWrittenThrough(&Arg, F))
                continue;

            ConstParamIssue issue;
            issue.funcName = F.getName().str();
            issue.paramName = dbg.name.empty() ? Arg.getName().str() : dbg.name;
            issue.line = dbg.line;
            issue.column = dbg.column;
            issue.pointerConstOnly =
                typeInfo.isPointer && typeInfo.pointerConst && !typeInfo.pointeeConst;
            issue.isReference = typeInfo.isReference;
            issue.isRvalueRef = typeInfo.isRvalueReference;

            std::string baseName = formatDITypeName(typeInfo.pointeeDisplayType);
            issue.currentType = buildTypeString(typeInfo, baseName, false, true, issue.paramName);
            if (typeInfo.isRvalueReference)
            {
                std::string valuePrefix = buildPointeeQualPrefix(typeInfo, false);
                std::string constRefPrefix = buildPointeeQualPrefix(typeInfo, true);
                issue.suggestedType = valuePrefix + baseName + " " + issue.paramName;
                issue.suggestedTypeAlt = constRefPrefix + baseName + " &" + issue.paramName;
            }
            else
            {
                issue.suggestedType =
                    buildTypeString(typeInfo, baseName, true, false, issue.paramName);
            }

            out.push_back(std::move(issue));
        }
    }

    // Forward declaration : essaie de retrouver une constante derrière une Value
    static const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V,
                                                         const llvm::Function& F);

    // Analyse intra-fonction pour détecter les allocations dynamiques sur la stack
    // (par exemple : int n = read(); char buf[n];)
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

    // Heuristic: compute an upper bound (if any) from an IntRange
    static std::optional<StackSize>
    getAllocaUpperBoundBytes(const llvm::AllocaInst* AI, const llvm::DataLayout& DL,
                             const std::map<const llvm::Value*, IntRange>& ranges)
    {
        using namespace llvm;

        const Value* sizeVal = AI->getArraySize();
        auto findRange = [&ranges](const Value* V) -> const IntRange*
        {
            auto it = ranges.find(V);
            if (it != ranges.end())
                return &it->second;
            return nullptr;
        };

        const IntRange* r = findRange(sizeVal);
        if (!r)
        {
            if (auto* LI = dyn_cast<LoadInst>(sizeVal))
            {
                const Value* ptr = LI->getPointerOperand();
                r = findRange(ptr);
            }
        }

        if (r && r->hasUpper && r->upper > 0)
        {
            StackSize elemSize = DL.getTypeAllocSize(AI->getAllocatedType());
            return static_cast<StackSize>(r->upper) * elemSize;
        }

        return std::nullopt;
    }

    // Analyze alloca/VLA uses whose size depends on a runtime value.
    static void analyzeAllocaUsageInFunction(llvm::Function& F, const llvm::DataLayout& DL,
                                             bool isRecursive, bool isInfiniteRecursive,
                                             std::vector<AllocaUsageIssue>& out)
    {
        using namespace llvm;

        if (F.isDeclaration())
            return;

        auto ranges = computeIntRangesFromICmps(F);

        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {
                auto* AI = dyn_cast<AllocaInst>(&I);
                if (!AI)
                    continue;

                // Only consider dynamic allocas: alloca(T, size) or VLA.
                if (!AI->isArrayAllocation())
                    continue;

                AllocaUsageIssue issue;
                issue.funcName = F.getName().str();
                issue.varName = deriveAllocaName(AI);
                issue.allocaInst = AI;
                issue.userControlled = isValueUserControlled(AI->getArraySize(), F);
                issue.isRecursive = isRecursive;
                issue.isInfiniteRecursive = isInfiniteRecursive;

                StackSize elemSize = DL.getTypeAllocSize(AI->getAllocatedType());
                const Value* arraySizeVal = AI->getArraySize();

                if (auto* C = dyn_cast<ConstantInt>(arraySizeVal))
                {
                    issue.sizeIsConst = true;
                    issue.sizeBytes = C->getZExtValue() * elemSize;
                }
                else if (auto* C = tryGetConstFromValue(arraySizeVal, F))
                {
                    issue.sizeIsConst = true;
                    issue.sizeBytes = C->getZExtValue() * elemSize;
                }
                else if (auto upper = getAllocaUpperBoundBytes(AI, DL, ranges))
                {
                    issue.hasUpperBound = true;
                    issue.upperBoundBytes = *upper;
                }

                out.push_back(std::move(issue));
            }
        }
    }

    // Forward declaration pour la résolution d'alloca de tableau depuis un pointeur
    static const llvm::AllocaInst* resolveArrayAllocaFromPointer(const llvm::Value* V,
                                                                 llvm::Function& F,
                                                                 std::vector<std::string>& path);

    // Analyse intra-fonction pour détecter des accès potentiellement hors bornes
    // sur des buffers alloués sur la stack (alloca).
    static void analyzeStackBufferOverflowsInFunction(llvm::Function& F,
                                                      std::vector<StackBufferOverflow>& out)
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
                            unsigned fieldIdx = static_cast<unsigned>(fieldIdxC->getZExtValue());
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
                                StackBufferOverflow report;
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
                                StackBufferOverflow report;
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
                            StackBufferOverflow report;
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
                            StackBufferOverflow report;
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
                            StackBufferOverflow report;
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
                            StackBufferOverflow report;
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

    // ============================================================================
    // Helpers
    // ============================================================================

    static void analyzeMemIntrinsicOverflowsInFunction(llvm::Function& F,
                                                       const llvm::DataLayout& DL,
                                                       std::vector<MemIntrinsicIssue>& out)
    {
        using namespace llvm;

        if (F.isDeclaration())
            return;

        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {

                // On s'intéresse uniquement aux appels (intrinsics ou libc)
                auto* CB = dyn_cast<CallBase>(&I);
                if (!CB)
                    continue;

                Function* callee = CB->getCalledFunction();
                if (!callee)
                    continue;

                StringRef name = callee->getName();

                enum class MemKind
                {
                    None,
                    MemCpy,
                    MemSet,
                    MemMove
                };
                MemKind kind = MemKind::None;

                // 1) Cas intrinsics LLVM: llvm.memcpy.*, llvm.memset.*, llvm.memmove.*
                if (auto* II = dyn_cast<IntrinsicInst>(CB))
                {
                    switch (II->getIntrinsicID())
                    {
                    case Intrinsic::memcpy:
                        kind = MemKind::MemCpy;
                        break;
                    case Intrinsic::memset:
                        kind = MemKind::MemSet;
                        break;
                    case Intrinsic::memmove:
                        kind = MemKind::MemMove;
                        break;
                    default:
                        break;
                    }
                }

                // 2) Cas appels libc classiques ou symboles similaires
                if (kind == MemKind::None)
                {
                    if (name == "memcpy" || name.contains("memcpy"))
                        kind = MemKind::MemCpy;
                    else if (name == "memset" || name.contains("memset"))
                        kind = MemKind::MemSet;
                    else if (name == "memmove" || name.contains("memmove"))
                        kind = MemKind::MemMove;
                }

                if (kind == MemKind::None)
                    continue;

                // On attend au moins 3 arguments: dest, src/val, len
                if (CB->arg_size() < 3)
                    continue;

                Value* dest = CB->getArgOperand(0);

                // Résolution heuristique : on enlève les casts/GEPI de surface
                // et on remonte jusqu'à une alloca éventuelle.
                const Value* cur = dest->stripPointerCasts();
                if (auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                {
                    cur = GEP->getPointerOperand();
                }
                const AllocaInst* AI = dyn_cast<AllocaInst>(cur);
                if (!AI)
                    continue;

                auto maybeSize = getAllocaTotalSizeBytes(AI, DL);
                if (!maybeSize)
                    continue;
                StackSize destBytes = *maybeSize;

                Value* lenV = CB->getArgOperand(2);
                auto* lenC = dyn_cast<ConstantInt>(lenV);
                if (!lenC)
                    continue; // pour l'instant, on ne traite que les tailles constantes

                uint64_t len = lenC->getZExtValue();
                if (len <= destBytes)
                    continue; // pas de débordement évident

                MemIntrinsicIssue issue;
                issue.funcName = F.getName().str();
                issue.varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                issue.destSizeBytes = destBytes;
                issue.lengthBytes = len;
                issue.inst = &I;

                switch (kind)
                {
                case MemKind::MemCpy:
                    issue.intrinsicName = "memcpy";
                    break;
                case MemKind::MemSet:
                    issue.intrinsicName = "memset";
                    break;
                case MemKind::MemMove:
                    issue.intrinsicName = "memmove";
                    break;
                default:
                    break;
                }

                out.push_back(std::move(issue));
            }
        }
    }

    // Appelle-t-on une autre fonction que soi-même ?
    static bool hasNonSelfCall(const llvm::Function& F)
    {
        const llvm::Function* Self = &F;

        for (const llvm::BasicBlock& BB : F)
        {
            for (const llvm::Instruction& I : BB)
            {

                const llvm::Function* Callee = nullptr;

                if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                {
                    Callee = CI->getCalledFunction();
                }
                else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                {
                    Callee = II->getCalledFunction();
                }

                if (Callee && !Callee->isDeclaration() && Callee != Self)
                {
                    return true; // appel vers une autre fonction
                }
            }
        }
        return false;
    }

    // ============================================================================
    //  Analyse locale de la stack (deux variantes)
    // ============================================================================

    static std::string deriveAllocaName(const llvm::AllocaInst* AI);

    static LocalStackInfo computeLocalStackBase(llvm::Function& F, const llvm::DataLayout& DL)
    {
        LocalStackInfo info;

        for (llvm::BasicBlock& BB : F)
        {
            for (llvm::Instruction& I : BB)
            {
                auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
                if (!alloca)
                    continue;

                llvm::Type* ty = alloca->getAllocatedType();
                StackSize count = 1;

                if (auto* CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize()))
                {
                    count = CI->getZExtValue();
                }
                else if (auto* C = tryGetConstFromValue(alloca->getArraySize(), F))
                {
                    count = C->getZExtValue();
                }
                else
                {
                    info.hasDynamicAlloca = true;
                    info.unknown = true;
                    continue;
                }

                StackSize size = DL.getTypeAllocSize(ty) * count;
                info.bytes += size;
                info.localAllocas.emplace_back(deriveAllocaName(alloca), size);
            }
        }

        return info;
    }

    // Mode IR pur : somme des allocas, alignée
    static LocalStackInfo computeLocalStackIR(llvm::Function& F, const llvm::DataLayout& DL)
    {
        LocalStackInfo info = computeLocalStackBase(F, DL);

        if (info.bytes == 0)
            return info;

        llvm::MaybeAlign MA = DL.getStackAlignment();
        unsigned stackAlign = MA ? MA->value() : 1u;

        if (stackAlign > 1)
            info.bytes = llvm::alignTo(info.bytes, stackAlign);

        return info;
    }

    // Mode ABI heuristique : frame minimale + overhead sur calls
    static LocalStackInfo computeLocalStackABI(llvm::Function& F, const llvm::DataLayout& DL)
    {
        LocalStackInfo info = computeLocalStackBase(F, DL);

        llvm::MaybeAlign MA = DL.getStackAlignment();
        unsigned stackAlign = MA ? MA->value() : 1u; // 16 sur beaucoup de cibles

        StackSize frameSize = info.bytes;

        if (stackAlign > 1)
            frameSize = llvm::alignTo(frameSize, stackAlign);

        if (!F.isDeclaration() && stackAlign > 1 && frameSize < stackAlign)
        {
            frameSize = stackAlign;
        }

        if (stackAlign > 1 && hasNonSelfCall(F))
        {
            frameSize = llvm::alignTo(frameSize + stackAlign, stackAlign);
        }

        info.bytes = frameSize;
        return info;
    }

    // Wrapper qui sélectionne le mode
    static LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                            AnalysisMode mode)
    {
        switch (mode)
        {
        case AnalysisMode::IR:
            return computeLocalStackIR(F, DL);
        case AnalysisMode::ABI:
            return computeLocalStackABI(F, DL);
        }
        return {};
    }

    // Threshold heuristic: consider an alloca "too large" if it consumes at least
    // 1/8 of the configured stack budget (8 MiB default), with a 64 KiB floor for
    // small limits.
    static StackSize computeAllocaLargeThreshold(const AnalysisConfig& config)
    {
        const StackSize defaultStack = 8ull * 1024ull * 1024ull;
        const StackSize minThreshold = 64ull * 1024ull; // 64 KiB

        StackSize base = config.stackLimit ? config.stackLimit : defaultStack;
        StackSize derived = base / 8;

        if (derived < minThreshold)
            derived = minThreshold;

        return derived;
    }

    // ============================================================================
    //  Construction du graphe d'appels (CallInst / InvokeInst)
    // ============================================================================

    static CallGraph buildCallGraph(llvm::Module& M)
    {
        CallGraph CG;

        for (llvm::Function& F : M)
        {
            if (F.isDeclaration())
                continue;

            auto& vec = CG[&F];

            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {

                    const llvm::Function* Callee = nullptr;

                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                    {
                        Callee = CI->getCalledFunction();
                    }
                    else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                    {
                        Callee = II->getCalledFunction();
                    }

                    if (Callee && !Callee->isDeclaration())
                    {
                        vec.push_back(Callee);
                    }
                }
            }
        }

        return CG;
    }

    // ============================================================================
    //  Propagation de la stack + détection de cycles
    // ============================================================================

    static StackEstimate
    dfsComputeStack(const llvm::Function* F, const CallGraph& CG,
                    const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                    std::map<const llvm::Function*, VisitState>& State, InternalAnalysisState& Res)
    {
        auto itState = State.find(F);
        if (itState != State.end())
        {
            if (itState->second == Visiting)
            {
                // Cycle détecté : on marque tous les noeuds actuellement en "Visiting"
                for (auto& p : State)
                {
                    if (p.second == Visiting)
                    {
                        Res.RecursiveFuncs.insert(p.first);
                    }
                }
                auto itLocal = LocalStack.find(F);
                if (itLocal != LocalStack.end())
                {
                    return StackEstimate{itLocal->second.bytes, itLocal->second.unknown};
                }
                return {};
            }
            else if (itState->second == Visited)
            {
                auto itTotal = Res.TotalStack.find(F);
                return (itTotal != Res.TotalStack.end()) ? itTotal->second : StackEstimate{};
            }
        }

        State[F] = Visiting;

        auto itLocal = LocalStack.find(F);
        StackEstimate local = {};
        if (itLocal != LocalStack.end())
        {
            local.bytes = itLocal->second.bytes;
            local.unknown = itLocal->second.unknown;
        }
        StackEstimate maxCallee = {};

        auto itCG = CG.find(F);
        if (itCG != CG.end())
        {
            for (const llvm::Function* Callee : itCG->second)
            {
                StackEstimate calleeStack = dfsComputeStack(Callee, CG, LocalStack, State, Res);
                if (calleeStack.bytes > maxCallee.bytes)
                    maxCallee.bytes = calleeStack.bytes;
                if (calleeStack.unknown)
                    maxCallee.unknown = true;
            }
        }

        StackEstimate total;
        total.bytes = local.bytes + maxCallee.bytes;
        total.unknown = local.unknown || maxCallee.unknown;
        Res.TotalStack[F] = total;
        State[F] = Visited;
        return total;
    }

    static InternalAnalysisState
    computeGlobalStackUsage(const CallGraph& CG,
                            const std::map<const llvm::Function*, LocalStackInfo>& LocalStack)
    {
        InternalAnalysisState Res;
        std::map<const llvm::Function*, VisitState> State;

        for (auto& p : LocalStack)
        {
            State[p.first] = NotVisited;
        }

        for (auto& p : LocalStack)
        {
            const llvm::Function* F = p.first;
            if (State[F] == NotVisited)
            {
                dfsComputeStack(F, CG, LocalStack, State, Res);
            }
        }

        return Res;
    }

    // ============================================================================
    //  Détection d’auto-récursion “infinie” (heuristique DominatorTree)
    // ============================================================================

    static bool detectInfiniteSelfRecursion(llvm::Function& F)
    {
        if (F.isDeclaration())
            return false;

        const llvm::Function* Self = &F;

        std::vector<llvm::BasicBlock*> SelfCallBlocks;

        for (llvm::BasicBlock& BB : F)
        {
            for (llvm::Instruction& I : BB)
            {
                const llvm::Function* Callee = nullptr;

                if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                {
                    Callee = CI->getCalledFunction();
                }
                else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                {
                    Callee = II->getCalledFunction();
                }

                if (Callee == Self)
                {
                    SelfCallBlocks.push_back(&BB);
                    break;
                }
            }
        }

        if (SelfCallBlocks.empty())
            return false;

        llvm::DominatorTree DT(F);

        bool hasReturn = false;

        for (llvm::BasicBlock& BB : F)
        {
            for (llvm::Instruction& I : BB)
            {
                if (llvm::isa<llvm::ReturnInst>(&I))
                {
                    hasReturn = true;

                    bool dominatedBySelfCall = false;
                    for (llvm::BasicBlock* SCB : SelfCallBlocks)
                    {
                        if (DT.dominates(SCB, &BB))
                        {
                            dominatedBySelfCall = true;
                            break;
                        }
                    }

                    if (!dominatedBySelfCall)
                        return false;
                }
            }
        }

        if (!hasReturn)
        {
            return true;
        }

        return true;
    }

    // HELPERS
    // Essaie de retrouver une alloca de tableau à partir d'un pointeur,
    // en suivant les bitcast, GEP(0,0), et un pattern simple de pointeur local :
    //   char test[10];
    //   char *ptr = test;
    //   ... load ptr ... ; gep -> ptr[i]
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
    } // namespace

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

    // Analyse intra-fonction pour détecter plusieurs stores dans un même buffer de stack.
    // Heuristique : on compte le nombre de StoreInst qui écrivent dans un GEP basé sur
    // une alloca de tableau sur la stack. Si une même alloca reçoit plus d'un store,
    // on émet un warning.
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
                const AllocaInst* AI = resolveArrayAllocaFromPointer(basePtr, F, dummyAliasPath);
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

    // HELPERS
    static const llvm::ConstantInt* tryGetConstFromValue(const llvm::Value* V,
                                                         const llvm::Function& F)
    {
        using namespace llvm;

        // On enlève d'abord les cast (sext/zext/trunc, etc.) pour arriver
        // à la vraie valeur “de base”.
        const Value* cur = V;
        while (auto* cast = dyn_cast<const CastInst>(cur))
        {
            cur = cast->getOperand(0);
        }

        // Cas trivial : c'est déjà une constante entière.
        if (auto* C = dyn_cast<const ConstantInt>(cur))
            return C;

        // Cas -O0 typique : on compare un load d'une variable locale.
        auto* LI = dyn_cast<const LoadInst>(cur);
        if (!LI)
            return nullptr;

        const Value* ptr = LI->getPointerOperand();
        const ConstantInt* found = nullptr;

        // Version ultra-simple : on cherche un store de constante dans la fonction.
        for (const BasicBlock& BB : F)
        {
            for (const Instruction& I : BB)
            {
                auto* SI = dyn_cast<const StoreInst>(&I);
                if (!SI)
                    continue;
                if (SI->getPointerOperand() != ptr)
                    continue;
                if (auto* C = dyn_cast<const ConstantInt>(SI->getValueOperand()))
                {
                    // On garde la dernière constante trouvée (si plusieurs stores, c'est naïf).
                    found = C;
                }
            }
        }
        return found;
    }

    // ============================================================================
    //  API publique : analyzeModule / analyzeFile
    // ============================================================================

    namespace
    {
        static std::string getFunctionSourcePath(const llvm::Function& F)
        {
            if (auto* sp = F.getSubprogram())
            {
                if (auto* file = sp->getFile())
                {
                    std::string dir = file->getDirectory().str();
                    std::string name = file->getFilename().str();
                    if (!dir.empty())
                        return dir + "/" + name;
                    return name;
                }
            }
            return {};
        }

        static bool getFunctionSourceLocation(const llvm::Function& F, unsigned& line,
                                              unsigned& column)
        {
            line = 0;
            column = 0;

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    llvm::DebugLoc DL = I.getDebugLoc();
                    if (!DL)
                        continue;
                    line = DL.getLine();
                    column = DL.getCol();
                    if (line != 0)
                    {
                        if (column == 0)
                            column = 1;
                        return true;
                    }
                }
            }

            if (auto* sp = F.getSubprogram())
            {
                line = sp->getLine();
                if (line != 0)
                {
                    column = 1;
                    return true;
                }
            }

            return false;
        }

        static std::string buildMaxStackCallPath(const llvm::Function* F, const CallGraph& CG,
                                                 const InternalAnalysisState& state)
        {
            std::string path;
            std::set<const llvm::Function*> visited;
            const llvm::Function* current = F;

            while (current)
            {
                if (!visited.insert(current).second)
                    break;

                if (!path.empty())
                    path += " -> ";
                path += current->getName().str();

                const llvm::Function* bestCallee = nullptr;
                StackEstimate bestStack{};

                auto itCG = CG.find(current);
                if (itCG == CG.end())
                    break;

                for (const llvm::Function* callee : itCG->second)
                {
                    auto itTotal = state.TotalStack.find(callee);
                    StackEstimate est = (itTotal != state.TotalStack.end()) ? itTotal->second
                                                                            : StackEstimate{};
                    if (!bestCallee || est.bytes > bestStack.bytes)
                    {
                        bestCallee = callee;
                        bestStack = est;
                    }
                }

                if (!bestCallee || bestStack.bytes == 0)
                    break;

                current = bestCallee;
            }

            return path;
        }

        static std::string normalizePathForMatch(const std::string& input)
        {
            std::string out = input;
            for (char& c : out)
            {
                if (c == '\\')
                    c = '/';
            }
            const bool isAbs = !out.empty() && out.front() == '/';
            std::vector<std::string> parts;
            std::string cur;
            for (char c : out)
            {
                if (c == '/')
                {
                    if (!cur.empty())
                    {
                        if (cur == "..")
                        {
                            if (!parts.empty())
                                parts.pop_back();
                        }
                        else if (cur != ".")
                        {
                            parts.push_back(cur);
                        }
                        cur.clear();
                    }
                }
                else
                {
                    cur.push_back(c);
                }
            }
            if (!cur.empty())
            {
                if (cur == "..")
                {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else if (cur != ".")
                {
                    parts.push_back(cur);
                }
            }
            std::string norm = isAbs ? "/" : "";
            for (std::size_t i = 0; i < parts.size(); ++i)
            {
                norm += parts[i];
                if (i + 1 < parts.size())
                    norm += "/";
            }
            while (!norm.empty() && norm.back() == '/')
                norm.pop_back();
            return norm;
        }

        static std::string basenameOf(const std::string& path)
        {
            std::size_t pos = path.find_last_of('/');
            if (pos == std::string::npos)
                return path;
            if (pos + 1 >= path.size())
                return {};
            return path.substr(pos + 1);
        }

        static bool pathHasSuffix(const std::string& path, const std::string& suffix)
        {
            if (suffix.empty())
                return false;
            if (path.size() < suffix.size())
                return false;
            if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
                return false;
            if (path.size() == suffix.size())
                return true;
            return path[path.size() - suffix.size() - 1] == '/';
        }

        static bool pathHasPrefix(const std::string& path, const std::string& prefix)
        {
            if (prefix.empty())
                return false;
            if (path.size() < prefix.size())
                return false;
            if (path.compare(0, prefix.size(), prefix) != 0)
                return false;
            if (path.size() == prefix.size())
                return true;
            return path[prefix.size()] == '/';
        }

        static bool shouldIncludePath(const std::string& path, const AnalysisConfig& config)
        {
            if (config.onlyFiles.empty() && config.onlyDirs.empty())
                return true;
            if (path.empty())
                return false;

            const std::string normPath = normalizePathForMatch(path);

            for (const auto& file : config.onlyFiles)
            {
                const std::string normFile = normalizePathForMatch(file);
                if (normPath == normFile || pathHasSuffix(normPath, normFile))
                    return true;
                const std::string fileBase = basenameOf(normFile);
                if (!fileBase.empty() && basenameOf(normPath) == fileBase)
                    return true;
            }

            for (const auto& dir : config.onlyDirs)
            {
                const std::string normDir = normalizePathForMatch(dir);
                if (pathHasPrefix(normPath, normDir) || pathHasSuffix(normPath, normDir))
                    return true;
                const std::string needle = "/" + normDir + "/";
                if (normPath.find(needle) != std::string::npos)
                    return true;
            }

            return false;
        }

        static bool functionNameMatches(const llvm::Function& F, const AnalysisConfig& config)
        {
            if (config.onlyFunctions.empty())
                return true;

            auto itaniumBaseName = [](const std::string& symbol) -> std::string
            {
                if (symbol.rfind("_Z", 0) != 0)
                    return {};
                std::size_t i = 2;
                if (i < symbol.size() && symbol[i] == 'L')
                    ++i;
                if (i >= symbol.size() || !std::isdigit(static_cast<unsigned char>(symbol[i])))
                    return {};
                std::size_t len = 0;
                while (i < symbol.size() && std::isdigit(static_cast<unsigned char>(symbol[i])))
                {
                    len = len * 10 + static_cast<std::size_t>(symbol[i] - '0');
                    ++i;
                }
                if (len == 0 || i + len > symbol.size())
                    return {};
                return symbol.substr(i, len);
            };

            std::string name = F.getName().str();
            std::string demangledName;
            if (ctrace_tools::isMangled(name) || name.rfind("_Z", 0) == 0)
                demangledName = ctrace_tools::demangle(name.c_str());
            std::string demangledBase;
            if (!demangledName.empty())
            {
                std::size_t pos = demangledName.find('(');
                if (pos != std::string::npos && pos > 0)
                    demangledBase = demangledName.substr(0, pos);
            }
            std::string itaniumBase = itaniumBaseName(name);

            for (const auto& filter : config.onlyFunctions)
            {
                if (name == filter)
                    return true;
                if (!demangledName.empty() && demangledName == filter)
                    return true;
                if (!demangledBase.empty() && demangledBase == filter)
                    return true;
                if (!itaniumBase.empty() && itaniumBase == filter)
                    return true;
                if (ctrace_tools::isMangled(filter))
                {
                    std::string demangledFilter = ctrace_tools::demangle(filter.c_str());
                    if (!demangledName.empty() && demangledName == demangledFilter)
                        return true;
                    std::size_t pos = demangledFilter.find('(');
                    if (pos != std::string::npos && pos > 0)
                    {
                        if (demangledBase == demangledFilter.substr(0, pos))
                            return true;
                    }
                }
            }

            return false;
        }
    } // namespace

    AnalysisResult analyzeModule(llvm::Module& mod, const AnalysisConfig& config)
    {
        const llvm::DataLayout& DL = mod.getDataLayout();
        const bool hasPathFilter = !config.onlyFiles.empty() || !config.onlyDirs.empty();
        const bool hasFuncFilter = !config.onlyFunctions.empty();
        const bool hasFilter = hasPathFilter || hasFuncFilter;
        const std::string moduleSourcePath = mod.getSourceFileName();

        auto shouldAnalyzeFunction = [&](const llvm::Function& F) -> bool
        {
            if (!hasFilter)
                return true;
            if (hasFuncFilter && !functionNameMatches(F, config))
            {
                if (config.dumpFilter)
                {
                    llvm::errs() << "[filter] func=" << F.getName()
                                 << " file=<name-filter> keep=no\n";
                }
                return false;
            }
            if (!hasPathFilter)
                return true;
            std::string path = getFunctionSourcePath(F);
            std::string usedPath;
            bool decision = false;
            if (!path.empty())
            {
                usedPath = path;
                decision = shouldIncludePath(usedPath, config);
            }
            else
            {
                llvm::StringRef name = F.getName();
                if (name.starts_with("__") || name.starts_with("llvm.") ||
                    name.starts_with("clang."))
                {
                    decision = false;
                }
                else if (!moduleSourcePath.empty())
                {
                    usedPath = moduleSourcePath;
                    decision = shouldIncludePath(usedPath, config);
                }
                else
                {
                    decision = false;
                }
            }

            if (config.dumpFilter)
            {
                llvm::errs() << "[filter] func=" << F.getName() << " file=";
                if (usedPath.empty())
                    llvm::errs() << "<none>";
                else
                    llvm::errs() << usedPath;
                llvm::errs() << " keep=" << (decision ? "yes" : "no") << "\n";
            }

            return decision;
        };

        // 1) Stack locale par fonction
        std::map<const llvm::Function*, LocalStackInfo> LocalStack;
        std::map<std::string, std::pair<unsigned, unsigned>> functionLocations;
        std::map<std::string, std::string> functionCallPaths;
        std::map<std::string, std::vector<std::pair<std::string, StackSize>>> functionLocalAllocas;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            LocalStackInfo info = computeLocalStack(F, DL, config.mode);
            LocalStack[&F] = info;
        }

        // 2) Graphe d'appels
        CallGraph CG;
        if (!hasFilter)
        {
            CG = buildCallGraph(mod);
        }
        else
        {
            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyzeFunction(F))
                    continue;

                auto& vec = CG[&F];

                for (llvm::BasicBlock& BB : F)
                {
                    for (llvm::Instruction& I : BB)
                    {
                        const llvm::Function* Callee = nullptr;
                        if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                        {
                            Callee = CI->getCalledFunction();
                        }
                        else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                        {
                            Callee = II->getCalledFunction();
                        }

                        if (Callee && !Callee->isDeclaration() && shouldAnalyzeFunction(*Callee))
                        {
                            vec.push_back(Callee);
                        }
                    }
                }
            }
        }

        // 3) Propagation + détection de récursivité
        InternalAnalysisState state = computeGlobalStackUsage(CG, LocalStack);

        // 4) Détection d’auto-récursion “infinie” pour les fonctions récursives
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            const llvm::Function* Fn = &F;
            if (!state.RecursiveFuncs.count(Fn))
                continue;

            if (detectInfiniteSelfRecursion(F))
            {
                state.InfiniteRecursionFuncs.insert(Fn);
            }
        }

        // 5) Construction du résultat public
        AnalysisResult result;
        result.config = config;
        StackSize allocaLargeThreshold = computeAllocaLargeThreshold(config);

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;

            const llvm::Function* Fn = &F;

            LocalStackInfo localInfo;
            StackEstimate totalInfo;

            auto itLocal = LocalStack.find(Fn);
            if (itLocal != LocalStack.end())
                localInfo = itLocal->second;

            auto itTotal = state.TotalStack.find(Fn);
            if (itTotal != state.TotalStack.end())
                totalInfo = itTotal->second;

            FunctionResult fr;
            fr.name = F.getName().str();
            fr.filePath = getFunctionSourcePath(F);
            if (fr.filePath.empty() && !moduleSourcePath.empty())
                fr.filePath = moduleSourcePath;
            fr.localStack = localInfo.bytes;
            fr.localStackUnknown = localInfo.unknown;
            fr.maxStack = totalInfo.bytes;
            fr.maxStackUnknown = totalInfo.unknown;
            fr.hasDynamicAlloca = localInfo.hasDynamicAlloca;
            fr.isRecursive = state.RecursiveFuncs.count(Fn) != 0;
            fr.hasInfiniteSelfRecursion = state.InfiniteRecursionFuncs.count(Fn) != 0;
            fr.exceedsLimit = (!fr.maxStackUnknown && totalInfo.bytes > config.stackLimit);

            unsigned line = 0;
            unsigned column = 0;
            if (getFunctionSourceLocation(F, line, column))
            {
                functionLocations[fr.name] = {line, column};
            }
            if (!fr.isRecursive && totalInfo.bytes > localInfo.bytes)
            {
                std::string path = buildMaxStackCallPath(Fn, CG, state);
                if (!path.empty())
                    functionCallPaths[fr.name] = path;
            }
            if (!localInfo.localAllocas.empty())
            {
                functionLocalAllocas[fr.name] = localInfo.localAllocas;
            }

            result.functions.push_back(std::move(fr));
        }

        // 5b) Emit summary diagnostics for recursion/overflow flags (for JSON parity)
        for (const auto& fr : result.functions)
        {
            if (fr.isRecursive)
            {
                Diagnostic diag;
                diag.funcName = fr.name;
                diag.filePath = fr.filePath;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::None;
                diag.message = "  [!] recursive or mutually recursive function detected\n";
                result.diagnostics.push_back(std::move(diag));
            }

            if (fr.hasInfiniteSelfRecursion)
            {
                Diagnostic diag;
                diag.funcName = fr.name;
                diag.filePath = fr.filePath;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::None;
                diag.message = "  [!!!] unconditional self recursion detected (no base case)\n"
                               "       this will eventually overflow the stack at runtime\n";
                result.diagnostics.push_back(std::move(diag));
            }

            if (fr.exceedsLimit)
            {
                Diagnostic diag;
                diag.funcName = fr.name;
                diag.filePath = fr.filePath;
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::None;
                auto it = functionLocations.find(fr.name);
                if (it != functionLocations.end())
                {
                    diag.line = it->second.first;
                    diag.column = it->second.second;
                }
                std::string message;
                bool suppressLocation = false;
                StackSize maxCallee =
                    (fr.maxStack > fr.localStack) ? (fr.maxStack - fr.localStack) : 0;
                auto itLocals = functionLocalAllocas.find(fr.name);
                std::string aliasLine;
                if (fr.localStack >= maxCallee && itLocals != functionLocalAllocas.end())
                {
                    std::string localsDetails;
                    std::string singleName;
                    StackSize singleSize = 0;
                    for (const auto& entry : itLocals->second)
                    {
                        if (entry.first == "<unnamed>")
                            continue;
                        if (entry.second >= config.stackLimit && entry.second > singleSize)
                        {
                            singleName = entry.first;
                            singleSize = entry.second;
                        }
                    }
                    if (!singleName.empty())
                    {
                        aliasLine = "       alias path: " + singleName + "\n";
                    }
                    else if (!itLocals->second.empty())
                    {
                        localsDetails += "        locals: " + std::to_string(itLocals->second.size()) +
                                         " variables (total " + std::to_string(fr.localStack) +
                                         " bytes)\n";

                        std::vector<std::pair<std::string, StackSize>> named = itLocals->second;
                        named.erase(std::remove_if(named.begin(), named.end(),
                                                   [](const auto& v)
                                                   { return v.first == "<unnamed>"; }),
                                    named.end());
                        std::sort(named.begin(), named.end(),
                                  [](const auto& a, const auto& b)
                                  {
                                      if (a.second != b.second)
                                          return a.second > b.second;
                                      return a.first < b.first;
                                  });
                        if (!named.empty())
                        {
                            constexpr std::size_t kMaxLocalsForLocation = 5;
                            if (named.size() > kMaxLocalsForLocation)
                                suppressLocation = true;
                            std::string listLine = "        locals list: ";
                            for (std::size_t idx = 0; idx < named.size(); ++idx)
                            {
                                if (idx > 0)
                                    listLine += ", ";
                                listLine += named[idx].first + "(" +
                                            std::to_string(named[idx].second) + ")";
                            }
                            localsDetails += listLine + "\n";
                        }
                    }
                    if (!localsDetails.empty())
                        message += localsDetails;
                }
                auto itPath = functionCallPaths.find(fr.name);
                std::string suffix;
                if (itPath != functionCallPaths.end())
                {
                    suffix += "    path: " + itPath->second + "\n";
                }
                std::string mainLine = "  [!] potential stack overflow: exceeds limit of " +
                                       std::to_string(config.stackLimit) + " bytes\n";
                message = mainLine + aliasLine + suffix + message;
                if (suppressLocation)
                {
                    diag.line = 0;
                    diag.column = 0;
                }
                diag.message = std::move(message);
                result.diagnostics.push_back(std::move(diag));
            }
        }

        // 6) Détection des dépassements de buffer sur la stack (analyse intra-fonction)
        std::vector<StackBufferOverflow> bufferIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeStackBufferOverflowsInFunction(F, bufferIssues);
        }

        // 7) Affichage des problèmes détectés (pour l'instant, sortie directe)
        for (const auto& issue : bufferIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            unsigned startLine = 0;
            unsigned startColumn = 0;
            unsigned endLine = 0;
            unsigned endColumn = 0;
            bool haveLoc = false;

            if (issue.inst)
            {
                llvm::DebugLoc DL = issue.inst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    startLine = DL.getLine();

                    startColumn = DL.getCol();
                    column = DL.getCol();

                    // By default, same as start
                    endLine = DL.getLine();
                    endColumn = DL.getCol();
                    haveLoc = true;
                    if (auto* loc = DL.get())
                    {
                        if (auto* scope = llvm::dyn_cast<llvm::DILocation>(loc))
                        {
                            if (scope->getColumn() != 0)
                            {
                                endColumn = scope->getColumn() + 1;
                            }
                        }
                    }
                }
            }

            bool isUnreachable = false;
            {
                using namespace llvm;

                if (issue.inst)
                {
                    auto* BB = issue.inst->getParent();

                    // Parcourt les prédécesseurs du bloc pour voir si certains
                    // ont une branche conditionnelle avec une condition constante.
                    for (auto* Pred : predecessors(BB))
                    {
                        auto* BI = dyn_cast<BranchInst>(Pred->getTerminator());
                        if (!BI || !BI->isConditional())
                            continue;

                        auto* CI = dyn_cast<ICmpInst>(BI->getCondition());
                        if (!CI)
                            continue;

                        const llvm::Function& Func = *issue.inst->getFunction();

                        auto* C0 = tryGetConstFromValue(CI->getOperand(0), Func);
                        auto* C1 = tryGetConstFromValue(CI->getOperand(1), Func);
                        if (!C0 || !C1)
                            continue;

                        // Évalue le résultat de l'ICmp pour ces constantes (implémentation maison).
                        bool condTrue = false;
                        auto pred = CI->getPredicate();
                        const auto& v0 = C0->getValue();
                        const auto& v1 = C1->getValue();

                        switch (pred)
                        {
                        case ICmpInst::ICMP_EQ:
                            condTrue = (v0 == v1);
                            break;
                        case ICmpInst::ICMP_NE:
                            condTrue = (v0 != v1);
                            break;
                        case ICmpInst::ICMP_SLT:
                            condTrue = v0.slt(v1);
                            break;
                        case ICmpInst::ICMP_SLE:
                            condTrue = v0.sle(v1);
                            break;
                        case ICmpInst::ICMP_SGT:
                            condTrue = v0.sgt(v1);
                            break;
                        case ICmpInst::ICMP_SGE:
                            condTrue = v0.sge(v1);
                            break;
                        case ICmpInst::ICMP_ULT:
                            condTrue = v0.ult(v1);
                            break;
                        case ICmpInst::ICMP_ULE:
                            condTrue = v0.ule(v1);
                            break;
                        case ICmpInst::ICMP_UGT:
                            condTrue = v0.ugt(v1);
                            break;
                        case ICmpInst::ICMP_UGE:
                            condTrue = v0.uge(v1);
                            break;
                        default:
                            // On ne traite pas d'autres prédicats exotiques ici
                            continue;
                        }

                        // Branchement du type:
                        //   br i1 %cond, label %then, label %else
                        // Successeur 0 pris si condTrue == true
                        // Successeur 1 pris si condTrue == false
                        if (BB == BI->getSuccessor(0) && condTrue == false)
                        {
                            // Le bloc "then" n'est jamais atteint.
                            isUnreachable = true;
                        }
                        else if (BB == BI->getSuccessor(1) && condTrue == true)
                        {
                            // Le bloc "else" n'est jamais atteint.
                            isUnreachable = true;
                        }
                    }
                }
            }

            std::ostringstream body;
            Diagnostic diag;

            if (issue.isLowerBoundViolation)
            {
                diag.errCode = DescriptiveErrorCode::NegativeStackIndex;
                body << "  [!!] potential negative index on variable '" << issue.varName
                     << "' (size " << issue.arraySize << ")\n";
                if (!issue.aliasPath.empty())
                {
                    body << "       alias path: " << issue.aliasPath << "\n";
                }
                body << "       inferred lower bound for index expression: " << issue.lowerBound
                     << " (index may be < 0)\n";
            }
            else
            {
                diag.errCode = DescriptiveErrorCode::StackBufferOverflow;
                body << "  [!!] potential stack buffer overflow on variable '" << issue.varName
                     << "' (size " << issue.arraySize << ")\n";
                if (!issue.aliasPath.empty())
                {
                    body << "       alias path: " << issue.aliasPath << "\n";
                }
                if (issue.indexIsConstant)
                {
                    body << "       constant index " << issue.indexOrUpperBound
                         << " is out of bounds (0.." << (issue.arraySize ? issue.arraySize - 1 : 0)
                         << ")\n";
                }
                else
                {
                    body << "       index variable may go up to " << issue.indexOrUpperBound
                         << " (array last valid index: "
                         << (issue.arraySize ? issue.arraySize - 1 : 0) << ")\n";
                }
            }

            if (issue.isWrite)
            {
                body << "       (this is a write access)\n";
            }
            else
            {
                body << "       (this is a read access)\n";
            }
            if (isUnreachable)
            {
                body << "       [info] this access appears unreachable at runtime "
                        "(condition is always false for this branch)\n";
            }

            diag.funcName = issue.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.startLine = haveLoc ? startLine : 0;
            diag.startColumn = haveLoc ? startColumn : 0;
            diag.endLine = haveLoc ? endLine : 0;
            diag.endColumn = haveLoc ? endColumn : 0;
            diag.severity = DiagnosticSeverity::Warning;
            diag.message = body.str();
            diag.variableAliasingVec = issue.aliasPathVec;
            result.diagnostics.push_back(std::move(diag));
        }

        // 8) Détection des allocations dynamiques sur la stack (VLA / alloca variable)
        std::vector<DynamicAllocaIssue> dynAllocaIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeDynamicAllocasInFunction(F, dynAllocaIssues);
        }

        // 9) Affichage des allocations dynamiques détectées
        for (const auto& d : dynAllocaIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            bool haveLoc = false;
            if (d.allocaInst)
            {
                llvm::DebugLoc DL = d.allocaInst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    column = DL.getCol();
                    haveLoc = true;
                }
            }

            std::ostringstream body;

            body << "  [!] dynamic stack allocation detected for variable '" << d.varName << "'\n";
            body << "       allocated type: " << d.typeName << "\n";
            body << "       size of this allocation is not compile-time constant "
                    "(VLA / variable alloca) and may lead to unbounded stack usage\n";

            Diagnostic diag;
            diag.funcName = d.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.severity = DiagnosticSeverity::Warning;
            diag.errCode = DescriptiveErrorCode::VLAUsage;
            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 10) Analyse des usages d'alloca (tainted / taille excessive)
        std::vector<AllocaUsageIssue> allocaUsageIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            bool isRec = state.RecursiveFuncs.count(&F) != 0;
            bool isInf = state.InfiniteRecursionFuncs.count(&F) != 0;
            analyzeAllocaUsageInFunction(F, DL, isRec, isInf, allocaUsageIssues);
        }

        for (const auto& a : allocaUsageIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            bool haveLoc = false;
            if (a.allocaInst)
            {
                llvm::DebugLoc DL = a.allocaInst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    column = DL.getCol();
                    haveLoc = true;
                }
            }

            bool isOversized = false;
            if (a.sizeIsConst && a.sizeBytes >= allocaLargeThreshold)
                isOversized = true;
            else if (a.hasUpperBound && a.upperBoundBytes >= allocaLargeThreshold)
                isOversized = true;
            else if (a.sizeIsConst && config.stackLimit != 0 && a.sizeBytes >= config.stackLimit)
                isOversized = true;

            std::ostringstream body;
            Diagnostic diag;
            diag.funcName = a.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;

            if (isOversized)
            {
                diag.severity = DiagnosticSeverity::Error;
                diag.errCode = DescriptiveErrorCode::AllocaTooLarge;
                body << "  [!!] large alloca on the stack for variable '" << a.varName << "'\n";
            }
            else if (a.userControlled)
            {
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::AllocaUserControlled;
                body << "  [!!] user-controlled alloca size for variable '" << a.varName << "'\n";
            }
            else
            {
                diag.severity = DiagnosticSeverity::Warning;
                diag.errCode = DescriptiveErrorCode::AllocaUsageWarning;
                body << "  [!] dynamic alloca on the stack for variable '" << a.varName << "'\n";
            }

            body << "       allocation performed via alloca/VLA; stack usage grows with runtime "
                    "value\n";

            if (a.sizeIsConst)
            {
                body << "       requested stack size: " << a.sizeBytes << " bytes\n";
            }
            else if (a.hasUpperBound)
            {
                body << "       inferred upper bound for size: " << a.upperBoundBytes << " bytes\n";
            }
            else
            {
                body << "       size is unbounded at compile time\n";
            }

            if (a.isInfiniteRecursive)
            {
                // Any alloca inside infinite recursion will blow the stack.
                diag.severity = DiagnosticSeverity::Error;
                body << "       function is infinitely recursive; this alloca runs at every frame "
                        "and guarantees stack overflow\n";
            }
            else if (a.isRecursive)
            {
                // Controlled recursion still compounds stack usage across frames.
                if (diag.severity != DiagnosticSeverity::Error && (isOversized || a.userControlled))
                {
                    diag.severity = DiagnosticSeverity::Error;
                }
                body << "       function is recursive; this allocation repeats at each recursion "
                        "depth and can exhaust the stack\n";
            }

            if (isOversized)
            {
                body << "       exceeds safety threshold of " << allocaLargeThreshold << " bytes";
                if (config.stackLimit != 0)
                {
                    body << " (stack limit: " << config.stackLimit << " bytes)";
                }
                body << "\n";
            }
            else if (a.userControlled)
            {
                body << "       size depends on user-controlled input "
                        "(function argument or non-local value)\n";
            }
            else
            {
                body << "       size does not appear user-controlled but remains "
                        "runtime-dependent\n";
            }

            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 11) Détection des débordements via memcpy/memset sur des buffers de stack
        std::vector<MemIntrinsicIssue> memIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeMemIntrinsicOverflowsInFunction(F, DL, memIssues);
        }

        for (const auto& m : memIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            bool haveLoc = false;
            if (m.inst)
            {
                llvm::DebugLoc DL = m.inst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    column = DL.getCol();
                    haveLoc = true;
                }
            }

            std::ostringstream body;

            body << "Function: " << m.funcName;
            if (haveLoc)
            {
                body << " (line " << line << ", column " << column << ")";
            }
            body << "\n";

            body << "  [!!] potential stack buffer overflow in " << m.intrinsicName
                 << " on variable '" << m.varName << "'\n";
            body << "       destination stack buffer size: " << m.destSizeBytes << " bytes\n";
            body << "       requested " << m.lengthBytes << " bytes to be copied/initialized\n";

            Diagnostic diag;
            diag.funcName = m.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.severity = DiagnosticSeverity::Warning;
            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 12) Détection de plusieurs stores dans un même buffer de stack
        std::vector<MultipleStoreIssue> multiStoreIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeMultipleStoresInFunction(F, multiStoreIssues);
        }

        for (const auto& ms : multiStoreIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            bool haveLoc = false;
            if (ms.allocaInst)
            {
                llvm::DebugLoc DL = ms.allocaInst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    column = DL.getCol();
                    haveLoc = true;
                }
            }

            std::ostringstream body;
            Diagnostic diag;

            body << "  [!Info] multiple stores to stack buffer '" << ms.varName
                 << "' in this function (" << ms.storeCount << " store instruction(s)";
            diag.errCode = DescriptiveErrorCode::MultipleStoresToStackBuffer;
            if (ms.distinctIndexCount > 0)
            {
                body << ", " << ms.distinctIndexCount << " distinct index expression(s)";
            }
            body << ")\n";

            if (ms.distinctIndexCount == 1)
            {
                body << "       all stores use the same index expression "
                        "(possible redundant or unintended overwrite)\n";
            }
            else if (ms.distinctIndexCount > 1)
            {
                body << "       stores use different index expressions; "
                        "verify indices are correct and non-overlapping\n";
            }

            diag.funcName = ms.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.severity = DiagnosticSeverity::Info;
            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 13) Détection des reconstructions invalides de pointeur de base (offsetof/container_of)
        std::vector<InvalidBaseReconstructionIssue> baseReconIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeInvalidBaseReconstructionsInFunction(F, DL, baseReconIssues);
        }

        for (const auto& br : baseReconIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            unsigned startLine = 0;
            unsigned startColumn = 0;
            unsigned endLine = 0;
            unsigned endColumn = 0;
            bool haveLoc = false;

            if (br.inst)
            {
                llvm::DebugLoc DL = br.inst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    startLine = DL.getLine();
                    startColumn = DL.getCol();
                    column = DL.getCol();
                    endLine = DL.getLine();
                    endColumn = DL.getCol();
                    haveLoc = true;

                    if (auto* loc = DL.get())
                    {
                        if (auto* scope = llvm::dyn_cast<llvm::DILocation>(loc))
                        {
                            if (scope->getColumn() != 0)
                            {
                                endColumn = scope->getColumn() + 1;
                            }
                        }
                    }
                }
            }

            std::ostringstream body;

            body << "  [!!] potential UB: invalid base reconstruction via offsetof/container_of\n";
            body << "       variable: '" << br.varName << "'\n";
            body << "       source member: " << br.sourceMember << "\n";
            body << "       offset applied: " << (br.offsetUsed >= 0 ? "+" : "") << br.offsetUsed
                 << " bytes\n";
            body << "       target type: " << br.targetType << "\n";

            if (br.isOutOfBounds)
            {
                body << "       [ERROR] derived pointer points OUTSIDE the valid object range\n";
                body << "               (this will cause undefined behavior if dereferenced)\n";
            }
            else
            {
                body << "       [WARNING] unable to verify that derived pointer points to a valid "
                        "object\n";
                body << "                 (potential undefined behavior if offset is incorrect)\n";
            }

            Diagnostic diag;
            diag.funcName = br.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.startLine = haveLoc ? startLine : 0;
            diag.startColumn = haveLoc ? startColumn : 0;
            diag.endLine = haveLoc ? endLine : 0;
            diag.endColumn = haveLoc ? endColumn : 0;
            diag.severity =
                br.isOutOfBounds ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
            diag.errCode = DescriptiveErrorCode::InvalidBaseReconstruction;
            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 14) Détection de fuite de pointeurs de stack (use-after-return potentiel)
        std::vector<StackPointerEscapeIssue> escapeIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeStackPointerEscapesInFunction(F, escapeIssues);
        }

        for (const auto& e : escapeIssues)
        {
            unsigned line = 0;
            unsigned column = 0;
            bool haveLoc = false;
            if (e.inst)
            {
                llvm::DebugLoc DL = e.inst->getDebugLoc();
                if (DL)
                {
                    line = DL.getLine();
                    column = DL.getCol();
                    haveLoc = true;
                }
            }

            std::ostringstream body;

            body << "  [!!] stack pointer escape: address of variable '" << e.varName
                 << "' escapes this function\n";

            if (e.escapeKind == "return")
            {
                body << "       escape via return statement "
                        "(pointer to stack returned to caller)\n";
            }
            else if (e.escapeKind == "store_global")
            {
                if (!e.targetName.empty())
                {
                    body << "       stored into global variable '" << e.targetName
                         << "' (pointer may be used after the function returns)\n";
                }
                else
                {
                    body << "       stored into a global variable "
                            "(pointer may be used after the function returns)\n";
                }
            }
            else if (e.escapeKind == "store_unknown")
            {
                body << "       stored through a non-local pointer "
                        "(e.g. via an out-parameter; pointer may outlive this function)\n";
                if (!e.targetName.empty())
                {
                    body << "       destination pointer/value name: '" << e.targetName << "'\n";
                }
            }
            else if (e.escapeKind == "call_callback")
            {
                body << "       address passed as argument to an indirect call "
                        "(callback may capture the pointer beyond this function)\n";
            }
            else if (e.escapeKind == "call_arg")
            {
                if (!e.targetName.empty())
                {
                    body << "       address passed as argument to function '" << e.targetName
                         << "' (callee may capture the pointer beyond this function)\n";
                }
                else
                {
                    body << "       address passed as argument to a function "
                            "(callee may capture the pointer beyond this function)\n";
                }
            }

            Diagnostic diag;
            diag.funcName = e.funcName;
            diag.line = haveLoc ? line : 0;
            diag.column = haveLoc ? column : 0;
            diag.severity = DiagnosticSeverity::Warning;
            diag.errCode = DescriptiveErrorCode::StackPointerEscape;
            diag.message = body.str();
            result.diagnostics.push_back(std::move(diag));
        }

        // 15) Const-correctness: parameters that can be made const
        std::vector<ConstParamIssue> constParamIssues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeConstParamsInFunction(F, constParamIssues);
        }

        for (const auto& cp : constParamIssues)
        {
            std::ostringstream body;
            Diagnostic diag;
            std::string displayFuncName = formatFunctionNameForMessage(cp.funcName);

            diag.severity = DiagnosticSeverity::Info;
            diag.errCode = DescriptiveErrorCode::ConstParameterNotModified;

            const char* prefix = "[!]";
            if (diag.severity == DiagnosticSeverity::Warning)
                prefix = "[!!]";
            else if (diag.severity == DiagnosticSeverity::Error)
                prefix = "[!!!]";

            const char* subLabel = "Pointer";
            if (cp.pointerConstOnly)
            {
                subLabel = "PointerConstOnly";
            }
            else if (cp.isReference)
            {
                subLabel = cp.isRvalueRef ? "ReferenceRvaluePreferValue" : "Reference";
            }

            if (cp.isRvalueRef)
            {
                body
                    << "  " << prefix << "ConstParameterNotModified." << subLabel << ": parameter '"
                    << cp.paramName << "' in function '" << displayFuncName
                    << "' is an rvalue reference and is never used to modify the referred object\n";
                body << "       consider passing by value (" << cp.suggestedType
                     << ") or const reference (" << cp.suggestedTypeAlt << ")\n";
                body << "       current type: " << cp.currentType << "\n";
            }
            else if (cp.pointerConstOnly)
            {
                body << "  " << prefix << "ConstParameterNotModified." << subLabel
                     << ": parameter '" << cp.paramName << "' in function '" << displayFuncName
                     << "' is declared '" << cp.currentType
                     << "' but the pointed object is never modified\n";
                body << "       consider '" << cp.suggestedType << "' for API const-correctness\n";
            }
            else
            {
                body << "  " << prefix << "ConstParameterNotModified." << subLabel
                     << ": parameter '" << cp.paramName << "' in function '" << displayFuncName
                     << "' is never used to modify the "
                     << (cp.isReference ? "referred" : "pointed") << " object\n";
            }

            if (!cp.isRvalueRef)
            {
                body << "       current type: " << cp.currentType << "\n";
                body << "       suggested type: " << cp.suggestedType << "\n";
            }

            diag.funcName = cp.funcName;
            diag.line = cp.line;
            diag.column = cp.column;
            diag.startLine = cp.line;
            diag.startColumn = cp.column;
            diag.endLine = cp.line;
            diag.endColumn = cp.column;
            diag.message = body.str();
            diag.ruleId = std::string("ConstParameterNotModified.") + subLabel;
            result.diagnostics.push_back(std::move(diag));
        }

        return result;
    }

    static LanguageType detectFromExtension(const std::string& path)
    {
        auto pos = path.find_last_of('.');
        if (pos == std::string::npos)
            return LanguageType::Unknown;

        std::string ext = path.substr(pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (ext == "ll")
            return LanguageType::LLVM_IR;

        if (ext == "c")
            return LanguageType::C;

        if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" || ext == "cp" ||
            ext == "C")
            return LanguageType::CXX;

        return LanguageType::Unknown;
    }

    LanguageType detectLanguageFromFile(const std::string& path, llvm::LLVMContext& ctx)
    {
        {
            llvm::SMDiagnostic diag;
            if (auto mod = llvm::parseIRFile(path, diag, ctx))
            {
                return LanguageType::LLVM_IR;
            }
        }

        return detectFromExtension(path);
    }

    AnalysisResult analyzeFile(const std::string& filename, const AnalysisConfig& config,
                               llvm::LLVMContext& ctx, llvm::SMDiagnostic& err)
    {

        LanguageType lang = detectLanguageFromFile(filename, ctx);
        std::unique_ptr<llvm::Module> mod;

        if (lang == LanguageType::Unknown)
        {
            std::cerr << "Unsupported input file type: " << filename << "\n";
            return AnalysisResult{config, {}};
        }

        // if (verboseLevel >= 1)
        //     std::cout << "Language: " << ctrace::stack::enumToString(lang) << "\n";

        if (lang != LanguageType::LLVM_IR)
        {
            // if (verboseLevel >= 1)
            //     std::cout << "Compiling source file to LLVM IR...\n";
            std::vector<std::string> args;
            args.push_back("-emit-llvm");
            args.push_back("-S");
            args.push_back("-g");
            if (lang == LanguageType::CXX)
            {
                args.push_back("-x");
                args.push_back("c++");
                args.push_back("-std=gnu++20");
            }
            for (const auto& extraArg : config.extraCompileArgs)
            {
                args.push_back(extraArg);
            }
            args.push_back("-fno-discard-value-names");
            args.push_back(filename);
            compilerlib::OutputMode mode = compilerlib::OutputMode::ToMemory;
            auto res = compilerlib::compile(args, mode);

            if (!res.success)
            {
                std::cerr << "Compilation failed:\n" << res.diagnostics << '\n';
                return AnalysisResult{config, {}};
            }

            if (res.llvmIR.empty())
            {
                std::cerr << "No LLVM IR produced by compilerlib::compile\n";
                return AnalysisResult{config, {}};
            }

            auto buffer = llvm::MemoryBuffer::getMemBuffer(res.llvmIR, "in_memory_ll");

            llvm::SMDiagnostic diag;
            mod = llvm::parseIR(buffer->getMemBufferRef(), diag, ctx);

            if (!mod)
            {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                diag.print("in_memory_ll", os);
                std::cerr << "Failed to parse in-memory LLVM IR:\n" << os.str();
                return AnalysisResult{config, {}};
            }
        }

        if (lang == LanguageType::LLVM_IR)
        {
            mod = llvm::parseIRFile(filename, err, ctx);
            if (!mod)
            {
                // on laisse err.print au caller si besoin
                return AnalysisResult{config, {}};
            }
        }
        AnalysisResult result = analyzeModule(*mod, config);
        for (auto& f : result.functions)
        {
            if (f.filePath.empty())
                f.filePath = filename;
        }
        for (auto& d : result.diagnostics)
        {
            if (d.filePath.empty())
                d.filePath = filename;
        }
        return result;
    }

    // ---------------------------------------------------------------------------
    // JSON / SARIF serialization helpers
    // ---------------------------------------------------------------------------

    namespace
    {

        // Petit helper pour échapper les chaînes JSON.
        static std::string jsonEscape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 16);
            for (char c : s)
            {
                switch (c)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '\"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
                }
            }
            return out;
        }

        // Old helper to convert DiagnosticSeverity to string, don't use it anymore.
        static const char* severityToJsonString(DiagnosticSeverity sev)
        {
            switch (sev)
            {
            case DiagnosticSeverity::Info:
                return "info";
            case DiagnosticSeverity::Warning:
                return "warning";
            case DiagnosticSeverity::Error:
                return "error";
            }
            return "info";
        }

        static const char* severityToSarifLevel(DiagnosticSeverity sev)
        {
            // SARIF levels: "none", "note", "warning", "error"
            switch (sev)
            {
            case DiagnosticSeverity::Info:
                return "note";
            case DiagnosticSeverity::Warning:
                return "warning";
            case DiagnosticSeverity::Error:
                return "error";
            }
            return "note";
        }

    } // anonymous namespace

    static std::string toJsonImpl(const AnalysisResult& result, const std::string* inputFile,
                                  const std::vector<std::string>* inputFiles)
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"meta\": {\n";
        os << "    \"tool\": \""
           << "ctrace-stack-analyzer"
           << "\",\n";
        if (inputFiles && !inputFiles->empty())
        {
            os << "    \"inputFiles\": [";
            for (std::size_t i = 0; i < inputFiles->size(); ++i)
            {
                os << "\"" << jsonEscape((*inputFiles)[i]) << "\"";
                if (i + 1 < inputFiles->size())
                    os << ", ";
            }
            os << "],\n";
        }
        else if (inputFile)
        {
            os << "    \"inputFile\": \"" << jsonEscape(*inputFile) << "\",\n";
        }
        os << "    \"mode\": \"" << (result.config.mode == AnalysisMode::IR ? "IR" : "ABI")
           << "\",\n";
        os << "    \"stackLimit\": " << result.config.stackLimit << ",\n";
        os << "    \"analysisTimeMs\": " << -1 << "\n";
        os << " },\n";

        // Fonctions
        os << "  \"functions\": [\n";
        for (std::size_t i = 0; i < result.functions.size(); ++i)
        {
            const auto& f = result.functions[i];
            os << "    {\n";
            std::string filePath = f.filePath;
            if (filePath.empty() && inputFile)
            {
                filePath = *inputFile;
            }
            os << "      \"file\": \"" << jsonEscape(filePath) << "\",\n";
            os << "      \"name\": \"" << jsonEscape(f.name) << "\",\n";
            os << "      \"localStack\": ";
            if (f.localStackUnknown)
            {
                os << "null";
            }
            else
            {
                os << f.localStack;
            }
            os << ",\n";
            os << "      \"localStackLowerBound\": ";
            if (f.localStackUnknown && f.localStack > 0)
            {
                os << f.localStack;
            }
            else
            {
                os << "null";
            }
            os << ",\n";
            os << "      \"localStackUnknown\": " << (f.localStackUnknown ? "true" : "false")
               << ",\n";
            os << "      \"maxStack\": ";
            if (f.maxStackUnknown)
            {
                os << "null";
            }
            else
            {
                os << f.maxStack;
            }
            os << ",\n";
            os << "      \"maxStackLowerBound\": ";
            if (f.maxStackUnknown && f.maxStack > 0)
            {
                os << f.maxStack;
            }
            else
            {
                os << "null";
            }
            os << ",\n";
            os << "      \"maxStackUnknown\": " << (f.maxStackUnknown ? "true" : "false") << ",\n";
            os << "      \"hasDynamicAlloca\": " << (f.hasDynamicAlloca ? "true" : "false")
               << ",\n";
            os << "      \"isRecursive\": " << (f.isRecursive ? "true" : "false") << ",\n";
            os << "      \"hasInfiniteSelfRecursion\": "
               << (f.hasInfiniteSelfRecursion ? "true" : "false") << ",\n";
            os << "      \"exceedsLimit\": " << (f.exceedsLimit ? "true" : "false") << "\n";
            os << "    }";
            if (i + 1 < result.functions.size())
                os << ",";
            os << "\n";
        }
        os << "  ],\n";

        // Diagnostics
        os << "  \"diagnostics\": [\n";
        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            const auto& d = result.diagnostics[i];
            os << "    {\n";
            os << "      \"id\": \"diag-" << (i + 1) << "\",\n";
            os << "      \"severity\": \"" << ctrace::stack::enumToString(d.severity) << "\",\n";
            const std::string ruleId =
                d.ruleId.empty() ? std::string(ctrace::stack::enumToString(d.errCode)) : d.ruleId;
            os << "      \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";

            std::string diagFilePath = d.filePath;
            if (diagFilePath.empty() && inputFile)
            {
                diagFilePath = *inputFile;
            }
            os << "      \"location\": {\n";
            os << "        \"file\": \"" << jsonEscape(diagFilePath) << "\",\n";
            os << "        \"function\": \"" << jsonEscape(d.funcName) << "\",\n";
            os << "        \"startLine\": " << d.line << ",\n";
            os << "        \"startColumn\": " << d.column << ",\n";
            os << "        \"endLine\": " << d.endLine << ",\n";
            os << "        \"endColumn\": " << d.endColumn << "\n";
            os << "      },\n";

            os << "      \"details\": {\n";
            os << "        \"message\": \"" << jsonEscape(d.message) << "\",\n";
            os << "        \"variableAliasing\": [";
            for (std::size_t j = 0; j < d.variableAliasingVec.size(); ++j)
            {
                os << "\"" << jsonEscape(d.variableAliasingVec[j]) << "\"";
                if (j + 1 < d.variableAliasingVec.size())
                    os << ", ";
            }
            os << "]\n";
            os << "      }\n"; // <-- ferme "details"
            os << "    }";     // <-- ferme le diagnostic
            if (i + 1 < result.diagnostics.size())
                os << ",";
            os << "\n";
        }
        os << "  ]\n";
        os << "}\n";
        return os.str();
    }

    std::string toJson(const AnalysisResult& result, const std::string& inputFile)
    {
        return toJsonImpl(result, &inputFile, nullptr);
    }

    std::string toJson(const AnalysisResult& result, const std::vector<std::string>& inputFiles)
    {
        return toJsonImpl(result, nullptr, &inputFiles);
    }

    std::string toSarif(const AnalysisResult& result, const std::string& inputFile,
                        const std::string& toolName, const std::string& toolVersion)
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"version\": \"2.1.0\",\n";
        os << "  \"$schema\": "
              "\"https://schemastore.azurewebsites.net/schemas/json/sarif-2.1.0.json\",\n";
        os << "  \"runs\": [\n";
        os << "    {\n";
        os << "      \"tool\": {\n";
        os << "        \"driver\": {\n";
        os << "          \"name\": \"" << jsonEscape(toolName) << "\",\n";
        os << "          \"version\": \"" << jsonEscape(toolVersion) << "\"\n";
        os << "        }\n";
        os << "      },\n";
        os << "      \"results\": [\n";

        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            const auto& d = result.diagnostics[i];
            os << "        {\n";
            // Pour le moment, un seul ruleId générique; tu pourras le spécialiser plus tard.
            const std::string ruleId =
                d.ruleId.empty() ? std::string(ctrace::stack::enumToString(d.errCode)) : d.ruleId;
            os << "          \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";
            os << "          \"level\": \"" << severityToSarifLevel(d.severity) << "\",\n";
            os << "          \"message\": { \"text\": \"" << jsonEscape(d.message) << "\" },\n";
            os << "          \"locations\": [\n";
            os << "            {\n";
            os << "              \"physicalLocation\": {\n";
            std::string diagFilePath = d.filePath.empty() ? inputFile : d.filePath;
            os << "                \"artifactLocation\": { \"uri\": \"" << jsonEscape(diagFilePath)
               << "\" },\n";
            os << "                \"region\": {\n";
            os << "                  \"startLine\": " << d.line << ",\n";
            os << "                  \"startColumn\": " << d.column << "\n";
            os << "                }\n";
            os << "              }\n";
            os << "            }\n";
            os << "          ]\n";
            os << "        }";
            if (i + 1 < result.diagnostics.size())
                os << ",";
            os << "\n";
        }

        os << "      ]\n";
        os << "    }\n";
        os << "  ]\n";
        os << "}\n";

        return os.str();
    }

} // namespace ctrace::stack
