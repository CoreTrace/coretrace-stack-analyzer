#include "analysis/DuplicateIfCondition.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <unordered_map>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Dominators.h>

#include "analysis/AnalyzerUtils.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        struct SourceLocation
        {
            std::string path;
            unsigned line = 0;
            unsigned column = 0;
        };

        struct SourceFileCache
        {
            std::unordered_map<std::string, std::vector<std::string>> files;
        };

        static SourceFileCache& getSourceCache()
        {
            static SourceFileCache cache;
            return cache;
        }

        static bool loadSourceFile(const std::string& path, std::vector<std::string>& lines)
        {
            std::ifstream in(path);
            if (!in)
                return false;
            std::string line;
            while (std::getline(in, line))
            {
                lines.push_back(line);
            }
            return true;
        }

        static const std::vector<std::string>* getSourceLines(const std::string& path)
        {
            if (path.empty())
                return nullptr;
            auto& cache = getSourceCache().files;
            auto it = cache.find(path);
            if (it != cache.end())
                return &it->second;
            std::vector<std::string> lines;
            if (!loadSourceFile(path, lines))
                return nullptr;
            auto [inserted, _] = cache.emplace(path, std::move(lines));
            return &inserted->second;
        }

        static bool isWordChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        }

        static std::string stripLineComment(const std::string& line)
        {
            bool inString = false;
            bool escape = false;
            for (std::size_t i = 0; i + 1 < line.size(); ++i)
            {
                char c = line[i];
                if (escape)
                {
                    escape = false;
                    continue;
                }
                if (c == '\\' && inString)
                {
                    escape = true;
                    continue;
                }
                if (c == '"')
                {
                    inString = !inString;
                    continue;
                }
                if (!inString && c == '/' && line[i + 1] == '/')
                {
                    return line.substr(0, i);
                }
            }
            return line;
        }

        static bool lineHasElseToken(const std::string& line)
        {
            std::size_t pos = 0;
            while ((pos = line.find("else", pos)) != std::string::npos)
            {
                bool leftOk = (pos == 0) || !isWordChar(line[pos - 1]);
                bool rightOk = (pos + 4 >= line.size()) || !isWordChar(line[pos + 4]);
                if (leftOk && rightOk)
                {
                    return true;
                }
                pos += 4;
            }
            return false;
        }

        static bool hasElseBetween(const std::vector<std::string>& lines, unsigned startLine,
                                   unsigned endLine, unsigned endColumn)
        {
            if (lines.empty() || startLine == 0 || endLine == 0)
                return false;
            if (startLine > endLine)
                std::swap(startLine, endLine);
            endLine = std::min(endLine, static_cast<unsigned>(lines.size()));
            startLine = std::min(startLine, endLine);

            for (unsigned lineNo = startLine; lineNo <= endLine; ++lineNo)
            {
                std::string view = stripLineComment(lines[lineNo - 1]);
                if (lineNo == endLine && endColumn > 0 && endColumn - 1 < view.size())
                {
                    view = view.substr(0, endColumn - 1);
                }
                if (lineHasElseToken(view))
                    return true;
            }
            return false;
        }

        static bool getSourceLocation(const llvm::Instruction* inst, SourceLocation& out)
        {
            if (!inst)
                return false;

            if (llvm::DebugLoc DL = inst->getDebugLoc())
            {
                out.line = DL.getLine();
                out.column = DL.getCol();
                std::string dir = DL->getDirectory().str();
                std::string file = DL->getFilename().str();
                if (!file.empty())
                {
                    if (!dir.empty())
                        out.path = dir + "/" + file;
                    else
                        out.path = file;
                }
            }

            if (out.path.empty())
            {
                out.path = getFunctionSourcePath(*inst->getFunction());
            }

            return !out.path.empty() && out.line != 0;
        }

        struct MemoryOperand
        {
            const llvm::Value* ptr = nullptr;
            bool precise = false; // true if we can reason about direct stores only
        };

        enum class ConditionKind
        {
            Invalid,
            ICmp,
            BoolValue
        };

        struct ConditionKey
        {
            ConditionKind kind = ConditionKind::Invalid;
            llvm::CmpInst::Predicate pred = llvm::CmpInst::BAD_ICMP_PREDICATE;
            llvm::Value* lhs = nullptr;
            llvm::Value* rhs = nullptr;
            llvm::Value* boolValue = nullptr;
            bool valid = false;
            llvm::SmallVector<MemoryOperand, 2> memoryOperands;
        };

        static llvm::Value* stripCasts(llvm::Value* v)
        {
            while (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
            {
                v = cast->getOperand(0);
            }
            return v;
        }

        static bool valuesEquivalent(const llvm::Value* a, const llvm::Value* b, int depth = 0)
        {
            if (a == b)
                return true;
            if (!a || !b)
                return false;
            if (depth > 6)
                return false;

            if (auto* la = llvm::dyn_cast<llvm::LoadInst>(a))
            {
                auto* lb = llvm::dyn_cast<llvm::LoadInst>(b);
                if (!lb)
                    return false;
                return valuesEquivalent(la->getPointerOperand()->stripPointerCasts(),
                                        lb->getPointerOperand()->stripPointerCasts(), depth + 1);
            }

            if (auto* ga = llvm::dyn_cast<llvm::GEPOperator>(a))
            {
                auto* gb = llvm::dyn_cast<llvm::GEPOperator>(b);
                if (!gb)
                    return false;
                if (ga->getNumIndices() != gb->getNumIndices())
                    return false;
                if (!valuesEquivalent(ga->getPointerOperand()->stripPointerCasts(),
                                      gb->getPointerOperand()->stripPointerCasts(), depth + 1))
                    return false;

                auto itA = ga->idx_begin();
                auto itB = gb->idx_begin();
                for (; itA != ga->idx_end(); ++itA, ++itB)
                {
                    auto* ca = llvm::dyn_cast<llvm::ConstantInt>(itA->get());
                    auto* cb = llvm::dyn_cast<llvm::ConstantInt>(itB->get());
                    if (!ca || !cb)
                        return false;
                    if (ca->getValue() != cb->getValue())
                        return false;
                }

                return true;
            }

            if (auto* opA = llvm::dyn_cast<llvm::Operator>(a))
            {
                auto* opB = llvm::dyn_cast<llvm::Operator>(b);
                if (!opB || opA->getOpcode() != opB->getOpcode())
                    return false;
                switch (opA->getOpcode())
                {
                case llvm::Instruction::BitCast:
                case llvm::Instruction::AddrSpaceCast:
                    return valuesEquivalent(opA->getOperand(0), opB->getOperand(0), depth + 1);
                default:
                    break;
                }
            }

            return false;
        }

        static bool isPrecisePointer(const llvm::Value* ptr)
        {
            using namespace llvm;
            if (!ptr)
                return false;
            const Value* base = ptr->stripPointerCasts();
            auto* allocaInst = dyn_cast<AllocaInst>(base);
            if (!allocaInst)
                return false;
            return !PointerMayBeCaptured(allocaInst, true, true);
        }

        static llvm::Value* canonicalizeOperand(llvm::Value* v, ConditionKey& key)
        {
            v = stripCasts(v);
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(v))
            {
                llvm::Value* ptr = load->getPointerOperand()->stripPointerCasts();
                key.memoryOperands.push_back({ptr, isPrecisePointer(ptr)});
                return ptr;
            }
            return v;
        }

        static void dedupeMemoryOperands(ConditionKey& key)
        {
            llvm::SmallPtrSet<const llvm::Value*, 4> seen;
            llvm::SmallVector<MemoryOperand, 2> deduped;
            deduped.reserve(key.memoryOperands.size());
            for (const auto& mem : key.memoryOperands)
            {
                if (!mem.ptr)
                    continue;
                if (seen.insert(mem.ptr).second)
                {
                    deduped.push_back(mem);
                }
            }
            key.memoryOperands.swap(deduped);
        }

        static ConditionKey buildConditionKey(llvm::Value* cond)
        {
            ConditionKey key;
            auto* cmp = llvm::dyn_cast<llvm::ICmpInst>(cond);
            if (!cmp)
            {
                llvm::Value* raw = stripCasts(cond);
                if (raw && raw->getType()->isIntegerTy())
                {
                    key.valid = true;
                    key.kind = ConditionKind::BoolValue;
                    key.boolValue = canonicalizeOperand(raw, key);
                    dedupeMemoryOperands(key);
                }
                return key;
            }

            key.valid = true;
            key.kind = ConditionKind::ICmp;
            key.pred = cmp->getPredicate();
            key.lhs = canonicalizeOperand(cmp->getOperand(0), key);
            key.rhs = canonicalizeOperand(cmp->getOperand(1), key);

            if (std::less<llvm::Value*>{}(key.rhs, key.lhs))
            {
                key.pred = llvm::CmpInst::getSwappedPredicate(key.pred);
                std::swap(key.lhs, key.rhs);
            }

            dedupeMemoryOperands(key);
            return key;
        }

        static bool conditionKeysEquivalent(const ConditionKey& a, const ConditionKey& b)
        {
            if (!a.valid || !b.valid)
                return false;
            if (a.kind != b.kind)
                return false;
            if (a.kind == ConditionKind::ICmp)
            {
                if (a.pred == b.pred && valuesEquivalent(a.lhs, b.lhs) &&
                    valuesEquivalent(a.rhs, b.rhs))
                    return true;
                if (llvm::CmpInst::getSwappedPredicate(a.pred) == b.pred &&
                    valuesEquivalent(a.lhs, b.rhs) && valuesEquivalent(a.rhs, b.lhs))
                    return true;
                return false;
            }
            if (a.kind == ConditionKind::BoolValue)
                return valuesEquivalent(a.boolValue, b.boolValue);
            return false;
        }

        static bool isInterferingWrite(const llvm::Instruction& I, const MemoryOperand& mem)
        {
            using namespace llvm;
            if (!I.mayWriteToMemory())
                return false;

            if (auto* store = dyn_cast<StoreInst>(&I))
            {
                const Value* ptr = store->getPointerOperand()->stripPointerCasts();
                return valuesEquivalent(ptr, mem.ptr);
            }

            if (auto* rmw = dyn_cast<AtomicRMWInst>(&I))
            {
                const Value* ptr = rmw->getPointerOperand()->stripPointerCasts();
                return valuesEquivalent(ptr, mem.ptr);
            }

            if (auto* cmpxchg = dyn_cast<AtomicCmpXchgInst>(&I))
            {
                const Value* ptr = cmpxchg->getPointerOperand()->stripPointerCasts();
                return valuesEquivalent(ptr, mem.ptr);
            }

            if (auto* memIntrinsic = dyn_cast<MemIntrinsic>(&I))
            {
                const Value* ptr = memIntrinsic->getRawDest()->stripPointerCasts();
                return valuesEquivalent(ptr, mem.ptr);
            }

            if (auto* call = dyn_cast<CallBase>(&I))
            {
                if (!call->mayWriteToMemory())
                    return false;
                if (!mem.precise)
                    return true;
                for (const Use& arg : call->args())
                {
                    const Value* argVal = arg.get();
                    if (!argVal || !argVal->getType()->isPointerTy())
                        continue;
                    if (valuesEquivalent(argVal->stripPointerCasts(), mem.ptr))
                        return true;
                }
                return false;
            }

            return !mem.precise;
        }

        static bool hasInterveningWrites(const llvm::Function& F, const llvm::DominatorTree& DT,
                                         const llvm::BasicBlock* pathBlock,
                                         const llvm::Instruction* at,
                                         const llvm::SmallVector<MemoryOperand, 2>& memoryOps)
        {
            if (memoryOps.empty() || !pathBlock || !at)
                return false;

            const llvm::BasicBlock* atBlock = at->getParent();

            for (const llvm::BasicBlock& BB : F)
            {
                if (!DT.dominates(pathBlock, &BB))
                    continue;
                if (&BB != atBlock && !DT.dominates(&BB, atBlock))
                    continue;

                for (const llvm::Instruction& I : BB)
                {
                    if (&BB == atBlock && &I == at)
                        break;
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(&I))
                        continue;
                    if (!I.mayWriteToMemory())
                        continue;
                    if (!llvm::isPotentiallyReachable(&I, at, nullptr, &DT))
                        continue;

                    for (const auto& mem : memoryOps)
                    {
                        if (isInterferingWrite(I, mem))
                            return true;
                    }
                }
            }

            return false;
        }

        static bool findDuplicateElseCondition(const llvm::BranchInst* branch,
                                               const llvm::DominatorTree& DT,
                                               DuplicateIfConditionIssue& out)
        {
            if (!branch || !branch->isConditional())
                return false;

            const llvm::BasicBlock* curBlock = branch->getParent();
            auto* node = DT.getNode(curBlock);
            if (!node)
                return false;

            ConditionKey curKey = buildConditionKey(branch->getCondition());
            if (!curKey.valid)
                return false;

            SourceLocation currentLoc;
            getSourceLocation(branch, currentLoc);

            for (auto* dom = node->getIDom(); dom; dom = dom->getIDom())
            {
                const llvm::BasicBlock* domBlock = dom->getBlock();
                if (!domBlock)
                    continue;
                auto* domTerm = llvm::dyn_cast<llvm::BranchInst>(domBlock->getTerminator());
                if (!domTerm || !domTerm->isConditional())
                    continue;

                const llvm::BasicBlock* falseSucc = domTerm->getSuccessor(1);
                if (!falseSucc || !DT.dominates(falseSucc, curBlock))
                    continue;

                ConditionKey domKey = buildConditionKey(domTerm->getCondition());
                if (!conditionKeysEquivalent(domKey, curKey))
                    continue;

                SourceLocation domLoc;
                if (!getSourceLocation(domTerm, domLoc))
                    continue;
                if (currentLoc.path.empty() || domLoc.path.empty() ||
                    currentLoc.path != domLoc.path)
                    continue;

                const auto* lines = getSourceLines(currentLoc.path);
                if (!lines ||
                    !hasElseBetween(*lines, domLoc.line, currentLoc.line, currentLoc.column))
                    continue;

                if (hasInterveningWrites(*curBlock->getParent(), DT, falseSucc, branch,
                                         curKey.memoryOperands))
                    continue;

                out.funcName = branch->getFunction()->getName().str();
                out.conditionInst = branch;
                return true;
            }

            return false;
        }

    } // namespace

    std::vector<DuplicateIfConditionIssue>
    analyzeDuplicateIfConditions(llvm::Module& mod,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<DuplicateIfConditionIssue> issues;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;

            llvm::DominatorTree DT(F);

            for (llvm::BasicBlock& BB : F)
            {
                auto* br = llvm::dyn_cast<llvm::BranchInst>(BB.getTerminator());
                if (!br || !br->isConditional())
                    continue;

                DuplicateIfConditionIssue issue;
                if (findDuplicateElseCondition(br, DT, issue))
                {
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
