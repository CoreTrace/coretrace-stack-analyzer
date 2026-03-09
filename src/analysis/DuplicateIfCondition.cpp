#include "analysis/DuplicateIfCondition.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <unordered_map>

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>

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

        static std::string trimAsciiWhitespace(std::string value)
        {
            std::size_t first = 0;
            while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])))
            {
                ++first;
            }

            std::size_t last = value.size();
            while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1])))
            {
                --last;
            }

            return value.substr(first, last - first);
        }

        static bool detectIfConditionNegation(const std::vector<std::string>& lines, unsigned line,
                                              unsigned column, bool& outNegated)
        {
            if (line == 0 || line > lines.size())
                return false;

            std::string view = stripLineComment(lines[line - 1]);
            if (view.empty())
                return false;

            std::size_t probe = view.size() - 1;
            if (column > 0)
                probe = std::min<std::size_t>(column - 1, probe);

            std::size_t openParen = view.rfind('(', probe);
            if (openParen == std::string::npos)
                return false;

            std::size_t closeParen = std::string::npos;
            unsigned depth = 1;
            for (std::size_t i = openParen + 1; i < view.size(); ++i)
            {
                if (view[i] == '(')
                {
                    ++depth;
                }
                else if (view[i] == ')')
                {
                    if (--depth == 0)
                    {
                        closeParen = i;
                        break;
                    }
                }
            }

            std::string condition = (closeParen == std::string::npos)
                                        ? view.substr(openParen + 1)
                                        : view.substr(openParen + 1, closeParen - openParen - 1);
            condition = trimAsciiWhitespace(condition);
            if (condition.empty())
                return false;

            std::string normalized;
            normalized.reserve(condition.size());
            for (char c : condition)
            {
                if (!std::isspace(static_cast<unsigned char>(c)))
                    normalized.push_back(c);
            }
            if (normalized.empty())
                return false;

            if (normalized[0] == '!')
            {
                outNegated = true;
                return true;
            }

            // Keep this filter intentionally conservative: when the source
            // condition is an explicit comparison (== / !=), do not infer
            // negation polarity from source text.
            if (normalized.find("==") != std::string::npos ||
                normalized.find("!=") != std::string::npos)
                return false;

            outNegated = false;
            return true;
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
            std::uint64_t precise : 1 = false; // true if we can reason about direct stores only
            std::uint64_t reservedFlags : 63 = 0;
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
            llvm::SmallVector<MemoryOperand, 2> memoryOperands;
            std::uint64_t valid : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;
        };

        struct ConditionAtom
        {
            ConditionKey key;
            std::uint64_t polarity : 1 = true;
            std::uint64_t reservedFlags : 63 = 0;
        };

        using ConditionSignature = llvm::SmallVector<ConditionAtom, 4>;

        struct DeterminismCache
        {
            std::unordered_map<const llvm::Function*, bool> memo;
            llvm::SmallPtrSet<const llvm::Function*, 16> visiting;
        };

        static DeterminismCache& getDeterminismCache()
        {
            static DeterminismCache cache;
            return cache;
        }

        static llvm::Value* stripCasts(llvm::Value* v)
        {
            while (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
            {
                v = cast->getOperand(0);
            }
            return v;
        }

        static const llvm::Value* stripCasts(const llvm::Value* v)
        {
            while (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
            {
                v = cast->getOperand(0);
            }
            return v;
        }

        static const llvm::Value* resolvePointerSource(const llvm::Value* ptr, unsigned depth = 0)
        {
            if (!ptr || depth > 6)
                return ptr;

            ptr = ptr->stripPointerCasts();
            auto* load = llvm::dyn_cast<llvm::LoadInst>(ptr);
            if (!load)
                return ptr;

            const llvm::Value* slotPtr = load->getPointerOperand()->stripPointerCasts();
            auto* slot = llvm::dyn_cast<llvm::AllocaInst>(slotPtr);
            if (!slot)
                return ptr;

            const llvm::Value* uniqueStoredPtr = nullptr;
            for (const llvm::Use& use : slot->uses())
            {
                const auto* inst = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                if (!inst)
                    continue;

                if (inst == load || llvm::isa<llvm::LoadInst>(inst) ||
                    llvm::isa<llvm::DbgInfoIntrinsic>(inst))
                {
                    continue;
                }

                if (auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(inst))
                {
                    const auto id = intrinsic->getIntrinsicID();
                    if (id == llvm::Intrinsic::lifetime_start ||
                        id == llvm::Intrinsic::lifetime_end)
                    {
                        continue;
                    }
                }

                auto* store = llvm::dyn_cast<llvm::StoreInst>(inst);
                if (!store || store->getPointerOperand()->stripPointerCasts() != slot)
                    return ptr;

                const llvm::Value* stored = store->getValueOperand()->stripPointerCasts();
                if (!stored->getType()->isPointerTy())
                    return ptr;

                if (!uniqueStoredPtr)
                {
                    uniqueStoredPtr = stored;
                    continue;
                }

                if (uniqueStoredPtr != stored)
                    return ptr;
            }

            if (!uniqueStoredPtr)
                return ptr;

            return resolvePointerSource(uniqueStoredPtr, depth + 1);
        }

        static const llvm::Value* getUnderlyingTrackedObject(const llvm::Value* ptr)
        {
            if (!ptr)
                return nullptr;
            const llvm::Value* resolved = resolvePointerSource(ptr);
            const llvm::Value* base = llvm::getUnderlyingObject(resolved->stripPointerCasts());
            if (!base)
                return nullptr;

            base = resolvePointerSource(base);
            return llvm::getUnderlyingObject(base->stripPointerCasts());
        }

        static bool isLocalWritableObject(const llvm::Value* ptr, const llvm::Function& F)
        {
            const llvm::Value* base = getUnderlyingTrackedObject(ptr);
            auto* allocaInst = llvm::dyn_cast_or_null<llvm::AllocaInst>(base);
            return allocaInst && allocaInst->getFunction() == &F;
        }

        static bool isAllowedReadObject(const llvm::Value* ptr, const llvm::Function& F)
        {
            const llvm::Value* base = getUnderlyingTrackedObject(ptr);
            if (!base)
                return false;
            if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(base))
                return allocaInst->getFunction() == &F;
            if (auto* arg = llvm::dyn_cast<llvm::Argument>(base))
                return arg->getParent() == &F;
            if (auto* gv = llvm::dyn_cast<llvm::GlobalVariable>(base))
                return gv->isConstant();
            return llvm::isa<llvm::Constant>(base);
        }

        static bool isKnownDeterministicDeclaration(const llvm::Function& F)
        {
            const llvm::StringRef name = F.getName();
            return name == "strcmp" || name == "strncmp" || name == "memcmp" || name == "strlen" ||
                   name == "strnlen" || name == "memchr" || name == "wcslen" || name == "wcscmp" ||
                   name == "wcsncmp";
        }

        static bool isFunctionDeterministic(const llvm::Function& F)
        {
            auto& cache = getDeterminismCache();
            auto it = cache.memo.find(&F);
            if (it != cache.memo.end())
                return it->second;
            if (!cache.visiting.insert(&F).second)
                return false;

            bool deterministic = true;
            if (F.isDeclaration())
            {
                deterministic = isKnownDeterministicDeclaration(F);
            }
            else
            {
                for (const llvm::BasicBlock& BB : F)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(&I))
                            continue;

                        if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&I))
                        {
                            if (load->isVolatile() ||
                                !isAllowedReadObject(load->getPointerOperand(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&I))
                        {
                            if (store->isVolatile() ||
                                !isLocalWritableObject(store->getPointerOperand(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* rmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&I))
                        {
                            if (!isLocalWritableObject(rmw->getPointerOperand(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* cmpxchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&I))
                        {
                            if (!isLocalWritableObject(cmpxchg->getPointerOperand(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* memTransfer = llvm::dyn_cast<llvm::MemTransferInst>(&I))
                        {
                            if (!isLocalWritableObject(memTransfer->getRawDest(), F) ||
                                !isAllowedReadObject(memTransfer->getRawSource(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* memSet = llvm::dyn_cast<llvm::MemSetInst>(&I))
                        {
                            if (!isLocalWritableObject(memSet->getRawDest(), F))
                            {
                                deterministic = false;
                                break;
                            }
                            continue;
                        }

                        if (auto* call = llvm::dyn_cast<llvm::CallBase>(&I))
                        {
                            if (call->isInlineAsm())
                            {
                                deterministic = false;
                                break;
                            }

                            const llvm::Function* callee = call->getCalledFunction();
                            if (!callee)
                            {
                                deterministic = false;
                                break;
                            }

                            if (callee->isIntrinsic())
                            {
                                const auto id = callee->getIntrinsicID();
                                if (id == llvm::Intrinsic::dbg_declare ||
                                    id == llvm::Intrinsic::dbg_value ||
                                    id == llvm::Intrinsic::dbg_label)
                                {
                                    continue;
                                }
                                if (id == llvm::Intrinsic::lifetime_start ||
                                    id == llvm::Intrinsic::lifetime_end ||
                                    id == llvm::Intrinsic::assume)
                                {
                                    continue;
                                }
                                if (!call->mayWriteToMemory())
                                    continue;
                                deterministic = false;
                                break;
                            }

                            if (callee->isDeclaration())
                            {
                                if (!(callee->doesNotReturn() ||
                                      isKnownDeterministicDeclaration(*callee)))
                                {
                                    deterministic = false;
                                    break;
                                }
                            }
                            else if (!isFunctionDeterministic(*callee))
                            {
                                deterministic = false;
                                break;
                            }

                            continue;
                        }

                        if (I.mayWriteToMemory())
                        {
                            deterministic = false;
                            break;
                        }
                    }

                    if (!deterministic)
                        break;
                }
            }

            cache.visiting.erase(&F);
            cache.memo[&F] = deterministic;
            return deterministic;
        }

        static bool isLikelyConstObserverCall(const llvm::CallBase* call,
                                              const llvm::Function& callee)
        {
            if (!call)
                return false;
            if (callee.isVarArg())
                return false;

            const llvm::StringRef name = callee.getName();
            // Itanium ABI: const member functions are encoded as "_ZNK...".
            if (!name.starts_with("_ZNK"))
                return false;

            // Member function call: first argument is 'this' pointer.
            if (call->arg_size() == 0 || !call->getArgOperand(0)->getType()->isPointerTy())
                return false;

            // Keep conservative behavior: additional pointer arguments may encode
            // out-params / writable aliasing that we cannot validate without body.
            for (unsigned i = 1; i < call->arg_size(); ++i)
            {
                if (call->getArgOperand(i)->getType()->isPointerTy())
                    return false;
            }

            return true;
        }

        static bool isDeterministicConditionCall(const llvm::CallBase* call)
        {
            if (!call)
                return false;
            const llvm::Function* callee = call->getCalledFunction();
            if (!callee)
                return false;

            if (callee->isDeclaration())
            {
                if (isKnownDeterministicDeclaration(*callee))
                    return true;
            }

            if (isFunctionDeterministic(*callee))
                return true;

            // Generic fallback for observer-like const methods. This recovers
            // stable duplicate-condition detection on O0 IR where deterministic
            // function bodies may be obscured by ABI lowering patterns.
            return isLikelyConstObserverCall(call, *callee);
        }

        static bool valuesEquivalent(const llvm::Value* a, const llvm::Value* b, int depth = 0);

        static bool callsEquivalent(const llvm::CallBase* a, const llvm::CallBase* b, int depth)
        {
            if (!a || !b)
                return false;
            if (a->arg_size() != b->arg_size())
                return false;

            const llvm::Function* calleeA = a->getCalledFunction();
            const llvm::Function* calleeB = b->getCalledFunction();
            if (!calleeA || !calleeB || calleeA != calleeB)
                return false;
            if (!isDeterministicConditionCall(a) || !isDeterministicConditionCall(b))
                return false;

            auto itA = a->arg_begin();
            auto itB = b->arg_begin();
            for (; itA != a->arg_end(); ++itA, ++itB)
            {
                const llvm::Value* argA = stripCasts(itA->get());
                const llvm::Value* argB = stripCasts(itB->get());
                if (!valuesEquivalent(argA, argB, depth + 1))
                    return false;
            }

            return true;
        }

        static bool valuesEquivalent(const llvm::Value* a, const llvm::Value* b, int depth)
        {
            if (a == b)
                return true;
            if (!a || !b)
                return false;
            if (depth > 6)
                return false;

            if (auto* ca = llvm::dyn_cast<llvm::CallBase>(a))
            {
                auto* cb = llvm::dyn_cast<llvm::CallBase>(b);
                if (!cb)
                    return false;
                return callsEquivalent(ca, cb, depth + 1);
            }

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
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(v))
            {
                for (const llvm::Use& arg : call->args())
                {
                    llvm::Value* argVal = stripCasts(arg.get());
                    if (!argVal || !argVal->getType()->isPointerTy())
                        continue;
                    llvm::Value* ptr = argVal->stripPointerCasts();
                    key.memoryOperands.push_back({ptr, isPrecisePointer(ptr)});
                }
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

        static bool normalizeBoolComparison(llvm::CmpInst::Predicate pred, llvm::Value* lhs,
                                            llvm::Value* rhs, llvm::Value*& outBoolValue)
        {
            if (pred != llvm::CmpInst::ICMP_EQ && pred != llvm::CmpInst::ICMP_NE)
                return false;

            auto* rhsConst = llvm::dyn_cast<llvm::ConstantInt>(rhs);
            if (!rhsConst)
            {
                auto* lhsConst = llvm::dyn_cast<llvm::ConstantInt>(lhs);
                if (!lhsConst)
                    return false;
                pred = llvm::CmpInst::getSwappedPredicate(pred);
                std::swap(lhs, rhs);
                rhsConst = lhsConst;
            }

            if (!lhs || !lhs->getType()->isIntegerTy())
                return false;

            const llvm::APInt& constant = rhsConst->getValue();
            if (constant.isZero())
            {
                outBoolValue = lhs;
                return true;
            }

            if (constant.isOne() && lhs->getType()->isIntegerTy(1))
            {
                outBoolValue = lhs;
                return true;
            }

            return false;
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
            key.pred = cmp->getPredicate();
            llvm::Value* rawLhs = stripCasts(cmp->getOperand(0));
            llvm::Value* rawRhs = stripCasts(cmp->getOperand(1));

            llvm::Value* normalizedBoolValue = nullptr;
            if (normalizeBoolComparison(key.pred, rawLhs, rawRhs, normalizedBoolValue))
            {
                key.kind = ConditionKind::BoolValue;
                key.boolValue = canonicalizeOperand(normalizedBoolValue, key);
                dedupeMemoryOperands(key);
                return key;
            }

            key.kind = ConditionKind::ICmp;
            key.lhs = canonicalizeOperand(rawLhs, key);
            key.rhs = canonicalizeOperand(rawRhs, key);

            if (std::less<llvm::Value*>{}(key.rhs, key.lhs))
            {
                key.pred = llvm::CmpInst::getSwappedPredicate(key.pred);
                std::swap(key.lhs, key.rhs);
            }

            dedupeMemoryOperands(key);
            return key;
        }

        static ConditionKey buildConditionKey(const llvm::Value* cond)
        {
            return buildConditionKey(const_cast<llvm::Value*>(cond));
        }

        static bool conditionKeysEquivalent(const ConditionKey& a, const ConditionKey& b);

        static const llvm::MDNode* getInstructionDebugScope(const llvm::Instruction* I)
        {
            if (!I)
                return nullptr;
            llvm::DebugLoc DL = I->getDebugLoc();
            if (!DL)
                return nullptr;
            return DL.getScope();
        }

        static bool haveCompatibleConditionScope(const llvm::Instruction* first,
                                                 const llvm::Instruction* second)
        {
            const llvm::MDNode* a = getInstructionDebugScope(first);
            const llvm::MDNode* b = getInstructionDebugScope(second);
            if (!a || !b)
                return false;
            return a == b;
        }

        static bool isShortCircuitContinuation(const llvm::BranchInst* branch, unsigned succIndex,
                                               const llvm::BranchInst*& nextBranch)
        {
            nextBranch = nullptr;
            if (!branch || !branch->isConditional() || succIndex > 1)
                return false;

            const llvm::BasicBlock* succ = branch->getSuccessor(succIndex);
            if (!succ || succ->getSinglePredecessor() != branch->getParent())
                return false;

            auto* succTerm = llvm::dyn_cast<llvm::BranchInst>(succ->getTerminator());
            if (!succTerm || !succTerm->isConditional())
                return false;

            if (!haveCompatibleConditionScope(branch, succTerm))
                return false;

            nextBranch = succTerm;
            return true;
        }

        static ConditionSignature buildConditionSignature(const llvm::BranchInst* branch)
        {
            ConditionSignature sig;
            if (!branch || !branch->isConditional())
                return sig;

            llvm::SmallPtrSet<const llvm::BasicBlock*, 8> seen;
            const llvm::BranchInst* cur = branch;
            for (unsigned depth = 0; cur && depth < 16; ++depth)
            {
                if (!seen.insert(cur->getParent()).second)
                    break;

                ConditionAtom atom;
                atom.key = buildConditionKey(cur->getCondition());
                if (!atom.key.valid)
                {
                    sig.clear();
                    return sig;
                }

                const llvm::BranchInst* nextOnTrue = nullptr;
                const llvm::BranchInst* nextOnFalse = nullptr;
                bool continueOnTrue = isShortCircuitContinuation(cur, 0, nextOnTrue);
                bool continueOnFalse = isShortCircuitContinuation(cur, 1, nextOnFalse);

                if (continueOnTrue && continueOnFalse)
                {
                    atom.polarity = true;
                    sig.push_back(std::move(atom));
                    break;
                }

                if (continueOnTrue)
                {
                    atom.polarity = true;
                    sig.push_back(std::move(atom));
                    cur = nextOnTrue;
                    continue;
                }

                if (continueOnFalse)
                {
                    atom.polarity = false;
                    sig.push_back(std::move(atom));
                    cur = nextOnFalse;
                    continue;
                }

                atom.polarity = true;
                sig.push_back(std::move(atom));
                break;
            }

            return sig;
        }

        static bool conditionSignaturesEquivalent(const ConditionSignature& a,
                                                  const ConditionSignature& b)
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].polarity != b[i].polarity)
                    return false;
                if (!conditionKeysEquivalent(a[i].key, b[i].key))
                    return false;
            }
            return true;
        }

        static llvm::SmallVector<MemoryOperand, 4>
        collectSignatureMemoryOperands(const ConditionSignature& sig)
        {
            llvm::SmallPtrSet<const llvm::Value*, 8> seen;
            llvm::SmallVector<MemoryOperand, 4> out;
            for (const auto& atom : sig)
            {
                for (const auto& mem : atom.key.memoryOperands)
                {
                    if (!mem.ptr)
                        continue;
                    if (seen.insert(mem.ptr).second)
                        out.push_back(mem);
                }
            }
            return out;
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

        static bool
        hasInterveningWrites(const llvm::Function& F, const llvm::DominatorTree& DT,
                             const llvm::BasicBlock* pathBlock, const llvm::Instruction* at,
                             llvm::ArrayRef<MemoryOperand> memoryOps,
                             const llvm::SmallPtrSet<const llvm::Instruction*, 8>& ignoredWrites)
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
                    if (ignoredWrites.count(&I))
                        continue;
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

        static llvm::SmallPtrSet<const llvm::Instruction*, 8>
        collectConditionInstructions(const llvm::BranchInst* branch)
        {
            llvm::SmallPtrSet<const llvm::Instruction*, 8> visited;
            if (!branch || !branch->isConditional())
                return visited;

            llvm::SmallVector<const llvm::Value*, 8> worklist;
            worklist.push_back(branch->getCondition());

            while (!worklist.empty())
            {
                const llvm::Value* value = worklist.pop_back_val();
                auto* inst = llvm::dyn_cast<llvm::Instruction>(value);
                if (!inst || inst->getParent() != branch->getParent())
                    continue;
                if (!visited.insert(inst).second)
                    continue;

                for (const llvm::Use& operand : inst->operands())
                {
                    worklist.push_back(operand.get());
                }
            }

            return visited;
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

            ConditionSignature curSig = buildConditionSignature(branch);
            if (curSig.empty())
                return false;
            llvm::SmallVector<MemoryOperand, 4> curMemoryOps =
                collectSignatureMemoryOperands(curSig);

            SourceLocation currentLoc;
            getSourceLocation(branch, currentLoc);
            llvm::SmallPtrSet<const llvm::Instruction*, 8> currentConditionInsts =
                collectConditionInstructions(branch);

            for (auto* dom = node->getIDom(); dom; dom = dom->getIDom())
            {
                const llvm::BasicBlock* domBlock = dom->getBlock();
                if (!domBlock)
                    continue;
                auto* domTerm = llvm::dyn_cast<llvm::BranchInst>(domBlock->getTerminator());
                if (!domTerm || !domTerm->isConditional())
                    continue;

                const llvm::BasicBlock* elsePathSucc = nullptr;
                unsigned dominatingSuccCount = 0;
                for (unsigned succIndex = 0; succIndex < domTerm->getNumSuccessors(); ++succIndex)
                {
                    const llvm::BasicBlock* succ = domTerm->getSuccessor(succIndex);
                    if (!succ || !DT.dominates(succ, curBlock))
                        continue;
                    elsePathSucc = succ;
                    ++dominatingSuccCount;
                }
                if (!elsePathSucc || dominatingSuccCount != 1)
                    continue;

                bool bypassesDominatingElsePath = false;
                llvm::SmallPtrSet<llvm::BasicBlock*, 1> exclusionSet;
                exclusionSet.insert(const_cast<llvm::BasicBlock*>(domBlock));
                for (unsigned succIndex = 0; succIndex < domTerm->getNumSuccessors(); ++succIndex)
                {
                    const llvm::BasicBlock* succ = domTerm->getSuccessor(succIndex);
                    if (!succ || succ == elsePathSucc)
                        continue;
                    if (llvm::isPotentiallyReachable(succ, curBlock, &exclusionSet, &DT))
                    {
                        bypassesDominatingElsePath = true;
                        break;
                    }
                }
                if (bypassesDominatingElsePath)
                    continue;

                ConditionSignature domSig = buildConditionSignature(domTerm);
                if (domSig.empty() || !conditionSignaturesEquivalent(domSig, curSig))
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

                bool domNegated = false;
                bool currentNegated = false;
                const bool domKnown =
                    detectIfConditionNegation(*lines, domLoc.line, domLoc.column, domNegated);
                const bool currentKnown = detectIfConditionNegation(
                    *lines, currentLoc.line, currentLoc.column, currentNegated);
                if (domKnown && currentKnown && domNegated != currentNegated)
                    continue;

                if (hasInterveningWrites(*curBlock->getParent(), DT, elsePathSucc, branch,
                                         curMemoryOps, currentConditionInsts))
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
        auto& cache = getDeterminismCache();
        cache.memo.clear();
        cache.visiting.clear();

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
