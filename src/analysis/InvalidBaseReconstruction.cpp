#include "analysis/InvalidBaseReconstruction.hpp"

#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/raw_ostream.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        constexpr std::size_t kMaxInvalidBaseWork = 200000;

        struct WorkBudget
        {
            std::size_t remaining = kMaxInvalidBaseWork;

            bool consume(std::size_t amount = 1)
            {
                if (remaining < amount)
                {
                    remaining = 0;
                    return false;
                }
                remaining -= amount;
                return true;
            }

            bool exhausted() const
            {
                return remaining == 0;
            }
        };

        static bool isLoadFromAlloca(const llvm::Value* V, const llvm::AllocaInst* AI)
        {
            if (!V || !AI)
                return false;
            const auto* LI = llvm::dyn_cast<llvm::LoadInst>(V);
            if (!LI)
                return false;
            const llvm::Value* Ptr = LI->getPointerOperand()->stripPointerCasts();
            return Ptr == AI;
        }

        static bool valueDependsOnAlloca(const llvm::Value* V, const llvm::AllocaInst* AI,
                                         llvm::SmallPtrSetImpl<const llvm::Value*>& visited)
        {
            using namespace llvm;

            if (!V || !AI)
                return false;
            if (visited.contains(V))
                return false;
            visited.insert(V);

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
            if (auto* CE = dyn_cast<ConstantExpr>(V))
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
            }

            return false;
        }

        struct PtrIntMatch
        {
            const llvm::Value* ptrOperand = nullptr;
            int64_t offset = 0;
            bool sawOffset = false;
        };

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
            return Cur ? Cur : V;
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

        static void collectPtrToIntMatches(const llvm::Value* V,
                                           llvm::SmallVectorImpl<PtrIntMatch>& out,
                                           WorkBudget& budget)
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
                if (!budget.consume())
                    return;
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

        struct PtrOrigin
        {
            const llvm::AllocaInst* alloca = nullptr;
            int64_t offset = 0;
        };

        static void collectPointerOrigins(const llvm::Value* V, const llvm::DataLayout& DL,
                                          llvm::SmallVectorImpl<PtrOrigin>& out, WorkBudget& budget)
        {
            using namespace llvm;

            SmallVector<std::pair<const Value*, int64_t>, 16> worklist;
            std::map<const Value*, std::set<int64_t>> visited;

            worklist.push_back({V, 0});
            recordVisitedOffset(visited, V, 0);

            while (!worklist.empty())
            {
                if (!budget.consume())
                    return;
                const Value* Cur = worklist.back().first;
                int64_t currentOffset = worklist.back().second;
                worklist.pop_back();

                if (auto* AI = dyn_cast<AllocaInst>(Cur))
                {
                    Type* allocaTy = AI->getAllocatedType();
                    if (allocaTy->isPointerTy())
                    {
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

                if (auto* ITP = dyn_cast<IntToPtrInst>(Cur))
                {
                    SmallVector<PtrIntMatch, 8> matches;
                    collectPtrToIntMatches(ITP->getOperand(0), matches, budget);
                    for (const auto& match : matches)
                    {
                        if (!match.ptrOperand)
                            continue;
                        int64_t newOffset = currentOffset + match.offset;
                        if (recordVisitedOffset(visited, match.ptrOperand, newOffset))
                            worklist.push_back({match.ptrOperand, newOffset});
                    }
                    continue;
                }

                if (auto* LI = dyn_cast<LoadInst>(Cur))
                {
                    const Value* PtrOp = LI->getPointerOperand()->stripPointerCasts();
                    const Value* basePtr = PtrOp;
                    int64_t baseOffset = 0;
                    if (getGEPConstantOffsetAndBase(basePtr, DL, baseOffset, basePtr))
                        basePtr = basePtr->stripPointerCasts();
                    if (auto* AI = dyn_cast<AllocaInst>(basePtr))
                    {
                        Type* allocTy = AI->getAllocatedType();
                        if (allocTy && allocTy->isPointerTy())
                        {
                            if (recordVisitedOffset(visited, PtrOp, currentOffset))
                                worklist.push_back({PtrOp, currentOffset});
                        }
                    }
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
                    else if (CE->getOpcode() == Instruction::IntToPtr)
                    {
                        SmallVector<PtrIntMatch, 8> matches;
                        collectPtrToIntMatches(CE->getOperand(0), matches, budget);
                        for (const auto& match : matches)
                        {
                            if (!match.ptrOperand)
                                continue;
                            int64_t newOffset = currentOffset + match.offset;
                            if (recordVisitedOffset(visited, match.ptrOperand, newOffset))
                                worklist.push_back({match.ptrOperand, newOffset});
                        }
                    }
                }
            }
        }

        static bool isPointerDereferencedOrUsed(const llvm::Value* V, WorkBudget& budget)
        {
            using namespace llvm;

            SmallVector<const Value*, 16> worklist;
            SmallPtrSet<const Value*, 32> visited;
            worklist.push_back(V);

            while (!worklist.empty())
            {
                if (!budget.consume())
                    return false;
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

        static std::optional<StackSize> getAllocaTotalSizeBytes(const llvm::AllocaInst* AI,
                                                                const llvm::DataLayout& DL)
        {
            using namespace llvm;

            Type* allocatedTy = AI->getAllocatedType();

            if (!AI->isArrayAllocation())
            {
                return DL.getTypeAllocSize(allocatedTy);
            }

            if (auto* C = dyn_cast<ConstantInt>(AI->getArraySize()))
            {
                uint64_t count = C->getZExtValue();
                uint64_t elemSize = DL.getTypeAllocSize(allocatedTy);
                return count * elemSize;
            }

            return std::nullopt;
        }

        static const llvm::StructType* getAllocaStructType(const llvm::AllocaInst* AI)
        {
            if (!AI)
                return nullptr;
            return llvm::dyn_cast<llvm::StructType>(AI->getAllocatedType());
        }

        static std::optional<unsigned> getStructMemberIndexAtOffset(const llvm::StructType* ST,
                                                                    const llvm::DataLayout& DL,
                                                                    uint64_t offset)
        {
            if (!ST)
                return std::nullopt;

            auto* mutableST = const_cast<llvm::StructType*>(ST);
            const llvm::StructLayout* layout = DL.getStructLayout(mutableST);
            const unsigned memberCount = ST->getNumElements();
            for (unsigned i = 0; i < memberCount; ++i)
            {
                uint64_t memberOffset = layout->getElementOffset(i);
                llvm::Type* memberTy = ST->getElementType(i);
                uint64_t memberSize = DL.getTypeAllocSize(memberTy);
                if (memberSize == 0)
                {
                    if (offset == memberOffset)
                        return i;
                    continue;
                }
                if (offset >= memberOffset && offset < memberOffset + memberSize)
                    return i;
            }

            return std::nullopt;
        }

        static bool isOffsetWithinSameAllocaMember(int64_t originOffset, int64_t resultOffset,
                                                   const llvm::StructType* structType,
                                                   uint64_t allocaSize, const llvm::DataLayout& DL)
        {
            if (originOffset < 0 || resultOffset < 0)
                return false;
            if (!structType)
                return false;
            uint64_t uOrigin = static_cast<uint64_t>(originOffset);
            uint64_t uResult = static_cast<uint64_t>(resultOffset);
            if (uOrigin >= allocaSize || uResult >= allocaSize)
                return false;
            auto originMember = getStructMemberIndexAtOffset(structType, DL, uOrigin);
            auto resultMember = getStructMemberIndexAtOffset(structType, DL, uResult);
            if (!originMember.has_value() || !resultMember.has_value())
                return false;
            return originMember.value() == resultMember.value();
        }

        static void analyzeInvalidBaseReconstructionsInFunction(
            llvm::Function& F, const llvm::DataLayout& DL,
            std::vector<InvalidBaseReconstructionIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            WorkBudget budget;
            struct AllocaInfo
            {
                std::string name;
                uint64_t size = 0;
                const StructType* structType = nullptr;
            };
            std::map<const AllocaInst*, AllocaInfo> allocaInfo;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;

                    std::optional<StackSize> sizeOpt = getAllocaTotalSizeBytes(AI, DL);
                    if (!sizeOpt.has_value())
                        continue;

                    std::string varName =
                        AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                    AllocaInfo info;
                    info.name = std::move(varName);
                    info.size = sizeOpt.value();
                    info.structType = getAllocaStructType(AI);
                    allocaInfo[AI] = std::move(info);
                }
            }

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    if (auto* ITP = dyn_cast<IntToPtrInst>(&I))
                    {
                        if (!isPointerDereferencedOrUsed(ITP, budget))
                            continue;

                        Value* IntVal = ITP->getOperand(0);

                        SmallVector<PtrIntMatch, 8> matches;
                        collectPtrToIntMatches(IntVal, matches, budget);
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
                            collectPointerOrigins(match.ptrOperand, DL, origins, budget);
                            if (origins.empty())
                                continue;

                            for (const auto& origin : origins)
                            {
                                auto it = allocaInfo.find(origin.alloca);
                                if (it == allocaInfo.end())
                                    continue;

                                const std::string& varName = it->second.name;
                                uint64_t allocaSize = it->second.size;
                                const StructType* structType = it->second.structType;

                                int64_t resultOffset = origin.offset + match.offset;
                                bool isOutOfBounds =
                                    (resultOffset < 0) ||
                                    (static_cast<uint64_t>(resultOffset) >= allocaSize);
                                bool isMemberOffset = isOffsetWithinSameAllocaMember(
                                    origin.offset, resultOffset, structType, allocaSize, DL);
                                bool allowMemberSuppression = match.offset != 0;

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
                                if (resultOffset != 0 &&
                                    !(allowMemberSuppression && isMemberOffset))
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

                    if (auto* GEP = dyn_cast<GetElementPtrInst>(&I))
                    {
                        if (!isPointerDereferencedOrUsed(GEP, budget))
                            continue;

                        int64_t gepOffset = 0;
                        const Value* PtrOp = nullptr;
                        if (!getGEPConstantOffsetAndBase(GEP, DL, gepOffset, PtrOp))
                            continue;

                        const Value* directBase = PtrOp ? PtrOp->stripPointerCasts() : nullptr;
                        const bool isDirectAllocaBase = directBase && isa<AllocaInst>(directBase);

                        SmallVector<PtrOrigin, 8> origins;
                        collectPointerOrigins(PtrOp, DL, origins, budget);
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
                            if (origin.offset == 0 && gepOffset >= 0 && isDirectAllocaBase)
                            {
                                continue;
                            }

                            auto it = allocaInfo.find(origin.alloca);
                            if (it == allocaInfo.end())
                                continue;

                            const std::string& varName = it->second.name;
                            uint64_t allocaSize = it->second.size;
                            const StructType* structType = it->second.structType;

                            int64_t resultOffset = origin.offset + gepOffset;
                            bool isOutOfBounds =
                                (resultOffset < 0) ||
                                (static_cast<uint64_t>(resultOffset) >= allocaSize);
                            bool isMemberOffset = isOffsetWithinSameAllocaMember(
                                origin.offset, resultOffset, structType, allocaSize, DL);
                            bool allowMemberSuppression = gepOffset != 0;

                            std::string targetType;
                            Type* targetTy = GEP->getType();
                            raw_string_ostream rso(targetType);
                            targetTy->print(rso);

                            auto& entry = agg[origin.alloca];
                            entry.memberOffsets.insert(origin.offset);
                            entry.anyOutOfBounds |= isOutOfBounds;
                            if (resultOffset != 0 && !(allowMemberSuppression && isMemberOffset))
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
    } // namespace

    std::vector<InvalidBaseReconstructionIssue> analyzeInvalidBaseReconstructions(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<InvalidBaseReconstructionIssue> issues;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeInvalidBaseReconstructionsInFunction(F, DL, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
