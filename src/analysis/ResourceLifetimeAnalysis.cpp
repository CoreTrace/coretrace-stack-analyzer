#include "analysis/ResourceLifetimeAnalysis.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/ErrorHandling.h>

#include <coretrace/logger.hpp>

#include "analysis/IRValueUtils.hpp"
#include "mangle.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool isStdLibCalleeName(llvm::StringRef name)
        {
            // Some symbols are emitted with the LLVM IR "\01" prefix to preserve
            // an exact assembler name; ignore it for prefix matching.
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();

            return name.starts_with("_ZNSt3__1") ||     // libc++ (Apple/LLVM) std::__1::
                   name.starts_with("_ZNSt7__cxx11") || // libstdc++ (GCC) std::__cxx11::
                   name.starts_with("_ZSt") ||          // libstdc++ free functions in std::
                   name.starts_with("_ZNSt") ||         // libstdc++ members in std::
                   name.starts_with("_ZNKSt");          // libstdc++ const members in std::
        }

        static bool shouldIgnoreStdLibSummaryPropagation(const llvm::Function& callee)
        {
            // Keep direct model rules active, but avoid propagating inter-proc
            // ownership summaries through stdlib implementation internals.
            return isStdLibCalleeName(callee.getName());
        }

        enum class RuleAction
        {
            AcquireOut,
            AcquireRet,
            ReleaseArg
        };

        enum class OwnershipState
        {
            Unknown,
            Owned,
            Released,
            Escaped
        };

        struct ResourceRule
        {
            RuleAction action = RuleAction::AcquireOut;
            std::string functionPattern;
            unsigned argIndex = 0;
            std::string resourceKind;
        };

        struct ResourceModel
        {
            std::vector<ResourceRule> rules;
        };

        struct MethodClassInfo
        {
            std::string className;
            std::string methodName;
            bool isCtor = false;
            bool isDtor = false;
            bool isLifecycleReleaseLike = false;
        };

        enum class StorageScope
        {
            Unknown,
            Local,
            Global,
            Argument,
            ThisField
        };

        struct StorageKey
        {
            StorageScope scope = StorageScope::Unknown;
            std::string key;
            std::string displayName;
            std::string className;
            std::uint64_t offset = 0;
            int argumentIndex = -1;
            const llvm::AllocaInst* localAlloca = nullptr;

            bool valid() const
            {
                return scope != StorageScope::Unknown && !key.empty();
            }
        };

        struct ParamLifetimeEffect
        {
            RuleAction action = RuleAction::AcquireOut;
            unsigned argIndex = 0;
            std::uint64_t offset = 0;
            bool viaPointerSlot = false;
            std::string resourceKind;
        };

        struct FunctionLifetimeSummary
        {
            std::vector<ParamLifetimeEffect> effects;
        };

        struct LocalHandleState
        {
            StorageKey storage;
            std::string resourceKind;
            std::string funcName;
            const llvm::Instruction* firstAcquireInst = nullptr;
            int acquires = 0;
            int releases = 0;
            bool escapesViaReturn = false;
            OwnershipState ownership = OwnershipState::Unknown;
        };

        struct ClassAcquireRecord
        {
            std::string className;
            std::string fieldName;
            std::string resourceKind;
            std::string funcName;
            const llvm::Instruction* inst = nullptr;
        };

        struct ClassLifecycleSummary
        {
            std::string className;
            std::unordered_map<std::string, ClassAcquireRecord> ctorAcquires;
            std::unordered_set<std::string> dtorReleases;
            std::unordered_set<std::string> lifecycleReleases;
            std::string dtorFuncName;
            const llvm::Instruction* dtorAnchor = nullptr;
        };

        static std::string trimCopy(const std::string& input)
        {
            std::size_t begin = 0;
            while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
                ++begin;
            std::size_t end = input.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
                --end;
            return input.substr(begin, end - begin);
        }

        static bool parseUnsignedIndex(const std::string& token, unsigned& out)
        {
            if (token.empty())
                return false;
            unsigned value = 0;
            for (char c : token)
            {
                if (!std::isdigit(static_cast<unsigned char>(c)))
                    return false;
                value = value * 10u + static_cast<unsigned>(c - '0');
            }
            out = value;
            return true;
        }

        static std::string toLowerAlphaNum(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            for (char c : input)
            {
                const unsigned char uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc))
                    out.push_back(static_cast<char>(std::tolower(uc)));
            }
            return out;
        }

        static bool isLifecycleReleaseLikeMethodName(const std::string& methodName)
        {
            if (methodName.empty())
                return false;

            const std::string normalized = toLowerAlphaNum(methodName);
            if (normalized.empty())
                return false;

            static constexpr std::array<const char*, 7> lifecycleVerbs = {
                "cleanup", "destroy", "dispose", "shutdown", "teardown", "release", "free"};
            for (const char* verb : lifecycleVerbs)
            {
                const std::size_t verbLen = std::char_traits<char>::length(verb);
                if (normalized.size() < verbLen)
                    continue;
                if (normalized.compare(0, verbLen, verb) == 0 ||
                    normalized.compare(normalized.size() - verbLen, verbLen, verb) == 0)
                {
                    return true;
                }
            }

            return false;
        }

        static bool globMatches(llvm::StringRef pattern, llvm::StringRef text)
        {
            std::size_t p = 0;
            std::size_t t = 0;
            std::size_t star = llvm::StringRef::npos;
            std::size_t match = 0;

            while (t < text.size())
            {
                if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
                {
                    ++p;
                    ++t;
                    continue;
                }
                if (p < pattern.size() && pattern[p] == '*')
                {
                    star = p++;
                    match = t;
                    continue;
                }
                if (star != llvm::StringRef::npos)
                {
                    p = star + 1;
                    t = ++match;
                    continue;
                }
                return false;
            }

            while (p < pattern.size() && pattern[p] == '*')
                ++p;
            return p == pattern.size();
        }

        static bool ruleMatchesFunction(const ResourceRule& rule, const llvm::Function& callee)
        {
            const std::string calleeName = callee.getName().str();
            const std::string demangled = ctrace_tools::demangle(calleeName.c_str());
            std::string demangledBase = demangled;
            if (const std::size_t pos = demangledBase.find('('); pos != std::string::npos)
                demangledBase = demangledBase.substr(0, pos);

            const llvm::StringRef pattern(rule.functionPattern);
            const bool hasGlob =
                pattern.contains('*') || pattern.contains('?') || pattern.contains('[');
            if (!hasGlob)
            {
                return rule.functionPattern == calleeName || rule.functionPattern == demangled ||
                       rule.functionPattern == demangledBase;
            }

            return globMatches(pattern, calleeName) || globMatches(pattern, demangled) ||
                   globMatches(pattern, demangledBase);
        }

        static bool parseResourceModel(const std::string& path, ResourceModel& out,
                                       std::string& error)
        {
            std::ifstream in(path);
            if (!in)
            {
                error = "cannot open model file: " + path;
                return false;
            }

            out.rules.clear();
            std::string line;
            unsigned lineNo = 0;
            while (std::getline(in, line))
            {
                ++lineNo;
                const std::size_t hashPos = line.find('#');
                if (hashPos != std::string::npos)
                    line.erase(hashPos);
                line = trimCopy(line);
                if (line.empty())
                    continue;

                std::istringstream iss(line);
                std::vector<std::string> tokens;
                std::string tok;
                while (iss >> tok)
                    tokens.push_back(tok);
                if (tokens.empty())
                    continue;

                ResourceRule rule;
                if (tokens[0] == "acquire_out")
                {
                    if (tokens.size() != 4)
                    {
                        error = "invalid acquire_out rule at line " + std::to_string(lineNo);
                        return false;
                    }
                    unsigned argIndex = 0;
                    if (!parseUnsignedIndex(tokens[2], argIndex))
                    {
                        error = "invalid argument index at line " + std::to_string(lineNo);
                        return false;
                    }
                    rule.action = RuleAction::AcquireOut;
                    rule.functionPattern = tokens[1];
                    rule.argIndex = argIndex;
                    rule.resourceKind = tokens[3];
                }
                else if (tokens[0] == "acquire_ret")
                {
                    if (tokens.size() != 3)
                    {
                        error = "invalid acquire_ret rule at line " + std::to_string(lineNo);
                        return false;
                    }
                    rule.action = RuleAction::AcquireRet;
                    rule.functionPattern = tokens[1];
                    rule.argIndex = 0;
                    rule.resourceKind = tokens[2];
                }
                else if (tokens[0] == "release_arg")
                {
                    if (tokens.size() != 4)
                    {
                        error = "invalid release_arg rule at line " + std::to_string(lineNo);
                        return false;
                    }
                    unsigned argIndex = 0;
                    if (!parseUnsignedIndex(tokens[2], argIndex))
                    {
                        error = "invalid argument index at line " + std::to_string(lineNo);
                        return false;
                    }
                    rule.action = RuleAction::ReleaseArg;
                    rule.functionPattern = tokens[1];
                    rule.argIndex = argIndex;
                    rule.resourceKind = tokens[3];
                }
                else
                {
                    error =
                        "unknown rule action '" + tokens[0] + "' at line " + std::to_string(lineNo);
                    return false;
                }

                out.rules.push_back(std::move(rule));
            }

            return true;
        }

        static MethodClassInfo describeMethodClass(const llvm::Function& F)
        {
            MethodClassInfo info;
            const std::string demangled = ctrace_tools::demangle(F.getName().str().c_str());
            const std::size_t parenPos = demangled.find('(');
            const std::string base = demangled.substr(0, parenPos);

            const std::size_t sepPos = base.rfind("::");
            if (sepPos == std::string::npos)
                return info;

            info.className = base.substr(0, sepPos);
            info.methodName = base.substr(sepPos + 2);
            const std::size_t classSep = info.className.rfind("::");
            const std::string shortClassName = (classSep == std::string::npos)
                                                   ? info.className
                                                   : info.className.substr(classSep + 2);

            if (info.methodName == shortClassName)
                info.isCtor = true;
            if (info.methodName == "~" + shortClassName)
                info.isDtor = true;
            if (!info.isCtor && !info.isDtor)
                info.isLifecycleReleaseLike = isLifecycleReleaseLikeMethodName(info.methodName);
            return info;
        }

        static std::string formatFieldName(std::uint64_t offset)
        {
            return "this+" + std::to_string(offset);
        }

        static const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* inputPtr)
        {
            const llvm::Value* current = inputPtr;
            for (unsigned depth = 0; depth < 4; ++depth)
            {
                const auto* LI = llvm::dyn_cast<llvm::LoadInst>(current->stripPointerCasts());
                if (!LI)
                    break;

                const auto* slot =
                    llvm::dyn_cast<llvm::AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
                if (!slot || !slot->isStaticAlloca() || !slot->getAllocatedType()->isPointerTy())
                {
                    break;
                }

                const llvm::StoreInst* uniqueStore = nullptr;
                bool unsafeUse = false;
                for (const llvm::Use& U : slot->uses())
                {
                    const auto* user = U.getUser();
                    if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (SI->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafeUse = true;
                            break;
                        }
                        if (uniqueStore && uniqueStore != SI)
                        {
                            uniqueStore = nullptr;
                            break;
                        }
                        uniqueStore = SI;
                        continue;
                    }
                    if (const auto* loadUser = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (loadUser->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafeUse = true;
                            break;
                        }
                        continue;
                    }
                    if (const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                    {
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(II) ||
                            llvm::isa<llvm::LifetimeIntrinsic>(II))
                        {
                            continue;
                        }
                    }
                    unsafeUse = true;
                    break;
                }

                if (unsafeUse || !uniqueStore)
                    break;

                const llvm::Value* storedPtr = uniqueStore->getValueOperand()->stripPointerCasts();
                if (!storedPtr->getType()->isPointerTy())
                    break;
                current = storedPtr;
            }
            return current;
        }

        static bool resolveCanonicalPointerBaseAndOffset(const llvm::Value* ptr,
                                                         const llvm::DataLayout& DL,
                                                         const llvm::Value*& canonical,
                                                         std::uint64_t& offset)
        {
            canonical = nullptr;
            offset = 0;
            if (!ptr || !ptr->getType()->isPointerTy())
                return false;

            const llvm::Value* stripped = peelPointerFromSingleStoreSlot(ptr)->stripPointerCasts();
            int64_t signedOffset = 0;
            const llvm::Value* base =
                llvm::GetPointerBaseWithConstantOffset(stripped, signedOffset, DL, true);
            if (!base)
            {
                base = llvm::getUnderlyingObject(stripped, 16);
                signedOffset = 0;
            }
            if (!base || signedOffset < 0)
                return false;

            canonical = peelPointerFromSingleStoreSlot(base)->stripPointerCasts();
            offset = static_cast<std::uint64_t>(signedOffset);
            return canonical != nullptr;
        }

        static const llvm::Value* resolveAllocaPointerShadowValue(const llvm::AllocaInst& slot,
                                                                  bool requireSimpleUses)
        {
            if (!slot.isStaticAlloca() || !slot.getAllocatedType()->isPointerTy())
                return nullptr;

            const llvm::Value* uniqueStored = nullptr;
            const llvm::StoreInst* uniqueStore = nullptr;
            for (const llvm::Use& U : slot.uses())
            {
                const auto* user = U.getUser();
                if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (SI->getPointerOperand()->stripPointerCasts() == &slot)
                    {
                        const llvm::Value* stored = SI->getValueOperand()->stripPointerCasts();
                        if (!stored->getType()->isPointerTy())
                            return nullptr;
                        if (uniqueStore && uniqueStore != SI)
                            return nullptr;
                        uniqueStore = SI;
                        if (uniqueStored && uniqueStored != stored)
                            return nullptr;
                        uniqueStored = stored;
                        continue;
                    }

                    if (requireSimpleUses && SI->getValueOperand()->stripPointerCasts() != &slot)
                    {
                        return nullptr;
                    }
                    continue;
                }

                if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (LI->getPointerOperand()->stripPointerCasts() != &slot)
                        return nullptr;
                    continue;
                }

                if (const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(II) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(II))
                    {
                        continue;
                    }
                }

                if (requireSimpleUses)
                    return nullptr;
            }
            return uniqueStored;
        }

        static const llvm::Argument* resolveAllocaArgumentShadow(const llvm::AllocaInst& slot,
                                                                 bool requireSimpleUses)
        {
            const llvm::Value* shadow = resolveAllocaPointerShadowValue(slot, requireSimpleUses);
            return shadow ? llvm::dyn_cast<llvm::Argument>(shadow) : nullptr;
        }

        static bool valueMayOriginateFromLocalAlloca(const llvm::Value* value,
                                                     const llvm::AllocaInst& sourceSlot,
                                                     unsigned depth);
        static bool valueMayBeAddressOfLocalAlloca(const llvm::Value* value,
                                                   const llvm::AllocaInst& sourceSlot,
                                                   unsigned depth);

        static bool allocaMayStoreValueFromLocalAlloca(const llvm::AllocaInst& storageSlot,
                                                       const llvm::AllocaInst& sourceSlot,
                                                       unsigned depth)
        {
            if (depth > 8)
                return false;
            if (&storageSlot == &sourceSlot)
                return true;

            for (const llvm::Use& U : storageSlot.uses())
            {
                const auto* user = U.getUser();
                if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (SI->getPointerOperand()->stripPointerCasts() != &storageSlot)
                        continue;
                    if (valueMayOriginateFromLocalAlloca(SI->getValueOperand(), sourceSlot,
                                                         depth + 1))
                    {
                        return true;
                    }
                    continue;
                }

                if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (LI->getPointerOperand()->stripPointerCasts() != &storageSlot)
                        return false;
                    continue;
                }

                if (const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(II) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(II))
                    {
                        continue;
                    }
                }
            }

            return false;
        }

        static bool valueMayOriginateFromLocalAlloca(const llvm::Value* value,
                                                     const llvm::AllocaInst& sourceSlot,
                                                     unsigned depth)
        {
            if (!value || depth > 8)
                return false;

            const llvm::Value* stripped = value->stripPointerCasts();
            if (stripped == &sourceSlot)
                return true;

            if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped))
            {
                const llvm::Value* ptr = LI->getPointerOperand()->stripPointerCasts();
                if (ptr == &sourceSlot)
                    return true;
                if (const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(ptr))
                {
                    return allocaMayStoreValueFromLocalAlloca(*slot, sourceSlot, depth + 1);
                }
                return false;
            }

            if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(stripped))
            {
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                {
                    if (valueMayOriginateFromLocalAlloca(PN->getIncomingValue(i), sourceSlot,
                                                         depth + 1))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(stripped))
            {
                return valueMayOriginateFromLocalAlloca(Sel->getTrueValue(), sourceSlot,
                                                        depth + 1) ||
                       valueMayOriginateFromLocalAlloca(Sel->getFalseValue(), sourceSlot,
                                                        depth + 1);
            }

            return false;
        }

        static bool valueMayBeAddressOfLocalAlloca(const llvm::Value* value,
                                                   const llvm::AllocaInst& sourceSlot,
                                                   unsigned depth)
        {
            if (!value || depth > 8)
                return false;

            const llvm::Value* stripped = value->stripPointerCasts();
            if (stripped == &sourceSlot)
                return true;

            if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped))
            {
                const llvm::Value* ptr = LI->getPointerOperand()->stripPointerCasts();
                if (ptr == &sourceSlot)
                    return false;

                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(ptr);
                if (!slot || !slot->isStaticAlloca())
                    return false;

                for (const llvm::Use& U : slot->uses())
                {
                    const auto* user = U.getUser();
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (!SI)
                        continue;
                    if (SI->getPointerOperand()->stripPointerCasts() != slot)
                        continue;
                    if (valueMayBeAddressOfLocalAlloca(SI->getValueOperand(), sourceSlot,
                                                       depth + 1))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(stripped))
            {
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                {
                    if (valueMayBeAddressOfLocalAlloca(PN->getIncomingValue(i), sourceSlot,
                                                       depth + 1))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(stripped))
            {
                return valueMayBeAddressOfLocalAlloca(Sel->getTrueValue(), sourceSlot, depth + 1) ||
                       valueMayBeAddressOfLocalAlloca(Sel->getFalseValue(), sourceSlot, depth + 1);
            }

            if (const auto* GEP = llvm::dyn_cast<llvm::GEPOperator>(stripped))
            {
                return valueMayBeAddressOfLocalAlloca(GEP->getPointerOperand(), sourceSlot,
                                                      depth + 1);
            }

            return false;
        }

        static StorageKey buildStorageKeyFromCanonical(const llvm::Value* canonical,
                                                       std::uint64_t offset,
                                                       const llvm::Function& F,
                                                       const MethodClassInfo& methodInfo)
        {
            StorageKey out;
            out.offset = offset;

            if (const auto* AI = llvm::dyn_cast<llvm::AllocaInst>(canonical))
            {
                if (offset == 0)
                {
                    if (const auto* argAlias = resolveAllocaArgumentShadow(*AI, true))
                    {
                        out.argumentIndex = static_cast<int>(argAlias->getArgNo());
                        if (argAlias->getArgNo() == 0 && !methodInfo.className.empty())
                        {
                            out.scope = StorageScope::ThisField;
                            out.className = methodInfo.className;
                            out.key = "this:" + methodInfo.className + ":" + std::to_string(offset);
                            out.displayName = formatFieldName(offset);
                            return out;
                        }

                        out.scope = StorageScope::Argument;
                        out.key = "arg:" + F.getName().str() + ":" +
                                  std::to_string(argAlias->getArgNo()) + ":" +
                                  std::to_string(offset);
                        out.displayName = argAlias->hasName()
                                              ? argAlias->getName().str()
                                              : ("arg" + std::to_string(argAlias->getArgNo()));
                        return out;
                    }
                }

                out.scope = StorageScope::Local;
                std::ostringstream key;
                key << "local:" << F.getName().str() << ":" << static_cast<const void*>(AI) << ":"
                    << offset;
                out.key = key.str();
                out.localAlloca = AI;
                std::string varName = deriveAllocaName(AI);
                if (varName.empty() || varName == "<unnamed>")
                    varName = "local";
                out.displayName =
                    (offset == 0) ? varName : (varName + "+" + std::to_string(offset));
                return out;
            }

            if (const auto* GV = llvm::dyn_cast<llvm::GlobalVariable>(canonical))
            {
                out.scope = StorageScope::Global;
                out.key = "global:" + GV->getName().str() + ":" + std::to_string(offset);
                out.displayName = GV->hasName() ? GV->getName().str() : std::string("global");
                return out;
            }

            if (const auto* Arg = llvm::dyn_cast<llvm::Argument>(canonical))
            {
                out.argumentIndex = static_cast<int>(Arg->getArgNo());
                if (Arg->getArgNo() == 0 && !methodInfo.className.empty())
                {
                    out.scope = StorageScope::ThisField;
                    out.className = methodInfo.className;
                    out.key = "this:" + methodInfo.className + ":" + std::to_string(offset);
                    out.displayName = formatFieldName(offset);
                    return out;
                }

                out.scope = StorageScope::Argument;
                out.key = "arg:" + F.getName().str() + ":" + std::to_string(Arg->getArgNo()) + ":" +
                          std::to_string(offset);
                out.displayName = Arg->hasName() ? Arg->getName().str()
                                                 : ("arg" + std::to_string(Arg->getArgNo()));
                return out;
            }

            return out;
        }

        static StorageKey resolvePointerStorageWithExtraOffset(const llvm::Value* ptr,
                                                               std::uint64_t extraOffset,
                                                               const llvm::Function& F,
                                                               const llvm::DataLayout& DL,
                                                               const MethodClassInfo& methodInfo)
        {
            StorageKey out;
            const llvm::Value* canonical = nullptr;
            std::uint64_t baseOffset = 0;
            if (!resolveCanonicalPointerBaseAndOffset(ptr, DL, canonical, baseOffset))
                return out;
            if (extraOffset > std::numeric_limits<std::uint64_t>::max() - baseOffset)
                return out;
            return buildStorageKeyFromCanonical(canonical, baseOffset + extraOffset, F, methodInfo);
        }

        static StorageKey resolvePointerStorage(const llvm::Value* ptr, const llvm::Function& F,
                                                const llvm::DataLayout& DL,
                                                const MethodClassInfo& methodInfo)
        {
            return resolvePointerStorageWithExtraOffset(ptr, 0, F, DL, methodInfo);
        }

        static const llvm::Function* resolveDirectCallee(const llvm::CallBase& CB);

        static void collectThisFieldOriginsFromValue(
            const llvm::Value* value, const llvm::Function& F, const llvm::DataLayout& DL,
            const MethodClassInfo& methodInfo, unsigned depth,
            llvm::SmallPtrSet<const llvm::Value*, 32>& visited,
            std::unordered_map<std::string, StorageKey>& thisFieldCandidates,
            bool& sawUnknownOrigin, bool& sawConflictingOrigin)
        {
            if (!value)
            {
                sawUnknownOrigin = true;
                return;
            }
            if (depth > 14)
            {
                sawUnknownOrigin = true;
                return;
            }

            const llvm::Value* stripped = value->stripPointerCasts();
            if (!visited.insert(stripped).second)
                return;

            if (llvm::isa<llvm::ConstantPointerNull>(stripped))
                return;

            if (stripped->getType()->isPointerTy())
            {
                StorageKey storage = resolvePointerStorage(stripped, F, DL, methodInfo);
                if (storage.valid())
                {
                    if (storage.scope == StorageScope::ThisField)
                    {
                        thisFieldCandidates.emplace(storage.key, storage);
                        return;
                    }

                    if (storage.scope == StorageScope::Local && storage.localAlloca)
                    {
                        bool sawMatchingStore = false;
                        for (const llvm::BasicBlock& BB : F)
                        {
                            for (const llvm::Instruction& I : BB)
                            {
                                const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                                if (!SI)
                                    continue;

                                const llvm::Value* canonical = nullptr;
                                std::uint64_t storeOffset = 0;
                                if (!resolveCanonicalPointerBaseAndOffset(
                                        SI->getPointerOperand(), DL, canonical, storeOffset))
                                {
                                    continue;
                                }
                                if (canonical != storage.localAlloca ||
                                    storeOffset != storage.offset)
                                    continue;

                                sawMatchingStore = true;
                                collectThisFieldOriginsFromValue(
                                    SI->getValueOperand(), F, DL, methodInfo, depth + 1, visited,
                                    thisFieldCandidates, sawUnknownOrigin, sawConflictingOrigin);
                            }
                        }

                        if (!sawMatchingStore)
                            sawUnknownOrigin = true;
                        return;
                    }

                    sawConflictingOrigin = true;
                    return;
                }
            }

            if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped))
            {
                collectThisFieldOriginsFromValue(LI->getPointerOperand(), F, DL, methodInfo,
                                                 depth + 1, visited, thisFieldCandidates,
                                                 sawUnknownOrigin, sawConflictingOrigin);
                return;
            }

            if (const auto* GEP = llvm::dyn_cast<llvm::GEPOperator>(stripped))
            {
                collectThisFieldOriginsFromValue(GEP->getPointerOperand(), F, DL, methodInfo,
                                                 depth + 1, visited, thisFieldCandidates,
                                                 sawUnknownOrigin, sawConflictingOrigin);
                return;
            }

            if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(stripped))
            {
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                {
                    collectThisFieldOriginsFromValue(PN->getIncomingValue(i), F, DL, methodInfo,
                                                     depth + 1, visited, thisFieldCandidates,
                                                     sawUnknownOrigin, sawConflictingOrigin);
                }
                return;
            }

            if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(stripped))
            {
                collectThisFieldOriginsFromValue(Sel->getTrueValue(), F, DL, methodInfo, depth + 1,
                                                 visited, thisFieldCandidates, sawUnknownOrigin,
                                                 sawConflictingOrigin);
                collectThisFieldOriginsFromValue(Sel->getFalseValue(), F, DL, methodInfo, depth + 1,
                                                 visited, thisFieldCandidates, sawUnknownOrigin,
                                                 sawConflictingOrigin);
                return;
            }

            if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(stripped))
            {
                const llvm::Function* callee = resolveDirectCallee(*CB);
                if (!callee || callee->isDeclaration())
                {
                    sawUnknownOrigin = true;
                    return;
                }

                bool sawPointerArg = false;
                for (unsigned i = 0; i < CB->arg_size(); ++i)
                {
                    const llvm::Value* arg = CB->getArgOperand(i);
                    if (!arg || !arg->getType()->isPointerTy())
                        continue;
                    sawPointerArg = true;
                    collectThisFieldOriginsFromValue(arg, F, DL, methodInfo, depth + 1, visited,
                                                     thisFieldCandidates, sawUnknownOrigin,
                                                     sawConflictingOrigin);
                }

                if (!sawPointerArg)
                    sawUnknownOrigin = true;
                return;
            }

            if (const auto* CastI = llvm::dyn_cast<llvm::CastInst>(stripped))
            {
                collectThisFieldOriginsFromValue(CastI->getOperand(0), F, DL, methodInfo, depth + 1,
                                                 visited, thisFieldCandidates, sawUnknownOrigin,
                                                 sawConflictingOrigin);
                return;
            }

            sawUnknownOrigin = true;
        }

        static StorageKey tryPromoteLocalStorageToThisField(const StorageKey& storage,
                                                            const llvm::Function& F,
                                                            const llvm::DataLayout& DL,
                                                            const MethodClassInfo& methodInfo)
        {
            StorageKey out;
            if (storage.scope != StorageScope::Local || !storage.localAlloca)
                return out;

            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            std::unordered_map<std::string, StorageKey> thisFieldCandidates;
            bool sawUnknownOrigin = false;
            bool sawConflictingOrigin = false;

            collectThisFieldOriginsFromValue(storage.localAlloca, F, DL, methodInfo, 0, visited,
                                             thisFieldCandidates, sawUnknownOrigin,
                                             sawConflictingOrigin);

            if (sawUnknownOrigin || sawConflictingOrigin || thisFieldCandidates.size() != 1)
                return out;

            return thisFieldCandidates.begin()->second;
        }

        static StorageKey resolveHandleStorage(const llvm::Value* handleValue,
                                               const llvm::Function& F, const llvm::DataLayout& DL,
                                               const MethodClassInfo& methodInfo)
        {
            if (!handleValue)
                return {};

            const llvm::Value* stripped = handleValue->stripPointerCasts();
            if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped))
            {
                StorageKey storage =
                    resolvePointerStorage(LI->getPointerOperand(), F, DL, methodInfo);
                StorageKey promoted = tryPromoteLocalStorageToThisField(storage, F, DL, methodInfo);
                return promoted.valid() ? promoted : storage;
            }

            if (stripped->getType()->isPointerTy())
            {
                StorageKey storage = resolvePointerStorage(stripped, F, DL, methodInfo);
                StorageKey promoted = tryPromoteLocalStorageToThisField(storage, F, DL, methodInfo);
                return promoted.valid() ? promoted : storage;
            }

            return {};
        }

        static void collectStoredDestinations(const llvm::Value* produced,
                                              std::vector<const llvm::Value*>& outPtrs)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            llvm::SmallVector<const llvm::Value*, 16> worklist;
            worklist.push_back(produced);

            while (!worklist.empty())
            {
                const llvm::Value* V = worklist.pop_back_val();
                if (!visited.insert(V).second)
                    continue;

                for (const llvm::Use& U : V->uses())
                {
                    const llvm::User* user = U.getUser();
                    if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (SI->getValueOperand() == V)
                            outPtrs.push_back(SI->getPointerOperand());
                        continue;
                    }

                    if (const auto* CI = llvm::dyn_cast<llvm::CastInst>(user))
                    {
                        worklist.push_back(CI);
                        continue;
                    }
                    if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(user))
                    {
                        worklist.push_back(PN);
                        continue;
                    }
                    if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        worklist.push_back(Sel);
                        continue;
                    }
                }
            }
        }

        static bool valueMayReachReturn(const llvm::Value* produced)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visitedValues;
            llvm::SmallPtrSet<const llvm::AllocaInst*, 16> visitedSlots;
            llvm::SmallVector<const llvm::Value*, 32> worklist;
            worklist.push_back(produced);

            while (!worklist.empty())
            {
                const llvm::Value* V = worklist.pop_back_val();
                if (!visitedValues.insert(V).second)
                    continue;

                for (const llvm::Use& U : V->uses())
                {
                    const llvm::User* user = U.getUser();
                    if (const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(user))
                    {
                        if (RI->getReturnValue() == V)
                            return true;
                        continue;
                    }
                    if (const auto* CI = llvm::dyn_cast<llvm::CastInst>(user))
                    {
                        worklist.push_back(CI);
                        continue;
                    }
                    if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(user))
                    {
                        worklist.push_back(PN);
                        continue;
                    }
                    if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        worklist.push_back(Sel);
                        continue;
                    }
                    if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (SI->getValueOperand() != V)
                            continue;
                        const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                            SI->getPointerOperand()->stripPointerCasts());
                        if (!slot || !slot->isStaticAlloca() ||
                            !slot->getAllocatedType()->isPointerTy())
                            continue;
                        if (!visitedSlots.insert(slot).second)
                            continue;
                        for (const llvm::Use& slotUse : slot->uses())
                        {
                            const auto* loadUser =
                                llvm::dyn_cast<llvm::LoadInst>(slotUse.getUser());
                            if (!loadUser ||
                                loadUser->getPointerOperand()->stripPointerCasts() != slot)
                            {
                                continue;
                            }
                            worklist.push_back(loadUser);
                        }
                    }
                }
            }

            return false;
        }

        static const llvm::Function* resolveDirectCallee(const llvm::CallBase& CB);
        static const llvm::Instruction* firstInstructionAnchor(const llvm::Function& F);

        static bool summaryStorageScopeAllowed(const StorageKey& storage)
        {
            return storage.scope == StorageScope::Argument ||
                   storage.scope == StorageScope::ThisField;
        }

        static std::string encodeSummaryEffectKey(const ParamLifetimeEffect& effect)
        {
            std::ostringstream oss;
            oss << static_cast<int>(effect.action) << "|" << effect.argIndex << "|" << effect.offset
                << "|" << (effect.viaPointerSlot ? 1 : 0) << "|" << effect.resourceKind;
            return oss.str();
        }

        static ResourceSummaryAction toPublicSummaryAction(RuleAction action)
        {
            switch (action)
            {
            case RuleAction::AcquireOut:
                return ResourceSummaryAction::AcquireOut;
            case RuleAction::AcquireRet:
                return ResourceSummaryAction::AcquireRet;
            case RuleAction::ReleaseArg:
                return ResourceSummaryAction::ReleaseArg;
            }
            llvm::report_fatal_error("Unhandled RuleAction in toPublicSummaryAction");
        }

        static RuleAction fromPublicSummaryAction(ResourceSummaryAction action)
        {
            switch (action)
            {
            case ResourceSummaryAction::AcquireOut:
                return RuleAction::AcquireOut;
            case ResourceSummaryAction::AcquireRet:
                return RuleAction::AcquireRet;
            case ResourceSummaryAction::ReleaseArg:
                return RuleAction::ReleaseArg;
            }
            llvm::report_fatal_error("Unhandled ResourceSummaryAction in fromPublicSummaryAction");
        }

        static std::unordered_map<std::string, FunctionLifetimeSummary>
        importExternalSummaryMap(const ResourceSummaryIndex* externalSummaries)
        {
            std::unordered_map<std::string, FunctionLifetimeSummary> out;
            if (!externalSummaries)
                return out;
            out.reserve(externalSummaries->functions.size());

            for (const auto& entry : externalSummaries->functions)
            {
                FunctionLifetimeSummary summary;
                summary.effects.reserve(entry.second.effects.size());
                for (const ResourceSummaryEffect& effect : entry.second.effects)
                {
                    ParamLifetimeEffect converted;
                    converted.action = fromPublicSummaryAction(effect.action);
                    converted.argIndex = effect.argIndex;
                    converted.offset = effect.offset;
                    converted.viaPointerSlot = effect.viaPointerSlot;
                    converted.resourceKind = effect.resourceKind;
                    summary.effects.push_back(std::move(converted));
                }
                std::sort(summary.effects.begin(), summary.effects.end(),
                          [](const ParamLifetimeEffect& lhs, const ParamLifetimeEffect& rhs)
                          { return encodeSummaryEffectKey(lhs) < encodeSummaryEffectKey(rhs); });
                out.emplace(ctrace_tools::canonicalizeMangledName(entry.first), std::move(summary));
            }
            return out;
        }

        static ResourceSummaryIndex exportSummaryIndexForModule(
            llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
            const std::unordered_map<const llvm::Function*, FunctionLifetimeSummary>& summaries)
        {
            ResourceSummaryIndex out;
            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (F.hasLocalLinkage())
                    continue;

                const auto it = summaries.find(&F);
                if (it == summaries.end())
                    continue;

                ResourceSummaryFunction publicSummary;
                publicSummary.effects.reserve(it->second.effects.size());
                for (const ParamLifetimeEffect& effect : it->second.effects)
                {
                    ResourceSummaryEffect exported;
                    exported.action = toPublicSummaryAction(effect.action);
                    exported.argIndex = effect.argIndex;
                    exported.offset = effect.offset;
                    exported.viaPointerSlot = effect.viaPointerSlot;
                    exported.resourceKind = effect.resourceKind;
                    publicSummary.effects.push_back(std::move(exported));
                }
                out.functions[ctrace_tools::canonicalizeMangledName(F.getName().str())] =
                    std::move(publicSummary);
            }
            return out;
        }

        static bool callMatchesAnyResourceRule(const ResourceModel& model,
                                               const llvm::Function& callee)
        {
            for (const ResourceRule& rule : model.rules)
            {
                if (ruleMatchesFunction(rule, callee))
                    return true;
            }
            return false;
        }

        static bool argumentMayCarryAddressOfLocal(const llvm::Function& F,
                                                   const llvm::DataLayout& DL,
                                                   const llvm::Value* argValue,
                                                   const llvm::AllocaInst& sourceSlot)
        {
            if (!argValue)
                return false;

            if (valueMayBeAddressOfLocalAlloca(argValue, sourceSlot, 0))
                return true;
            if (!argValue->getType()->isPointerTy())
                return false;

            const llvm::Value* argCanonical = nullptr;
            std::uint64_t argOffset = 0;
            if (!resolveCanonicalPointerBaseAndOffset(argValue, DL, argCanonical, argOffset))
                return false;
            if (argCanonical == &sourceSlot)
                return true;

            // Aggregate args can indirectly carry local addresses in their fields.
            // Scan stores into the same aggregate/object region.
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                    if (!SI)
                        continue;

                    const llvm::Value* storeCanonical = nullptr;
                    std::uint64_t storeOffset = 0;
                    if (!resolveCanonicalPointerBaseAndOffset(SI->getPointerOperand(), DL,
                                                              storeCanonical, storeOffset))
                    {
                        continue;
                    }
                    if (storeCanonical != argCanonical || storeOffset < argOffset)
                        continue;
                    if (valueMayBeAddressOfLocalAlloca(SI->getValueOperand(), sourceSlot, 0))
                        return true;
                }
            }
            return false;
        }

        static bool localAddressEscapesToUnmodeledCall(
            const llvm::Function& F, const llvm::AllocaInst& sourceSlot, const ResourceModel& model,
            const std::unordered_map<const llvm::Function*, FunctionLifetimeSummary>& summaries,
            const std::unordered_map<std::string, FunctionLifetimeSummary>* externalSummariesByName,
            const llvm::DataLayout& DL,
            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
        {
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;
                    if (llvm::isa<llvm::IntrinsicInst>(CB))
                        continue;

                    const llvm::Function* callee = resolveDirectCallee(*CB);
                    bool modeledCall = false;
                    if (callee)
                    {
                        if (callMatchesAnyResourceRule(model, *callee))
                        {
                            modeledCall = true;
                        }
                        else if (!callee->isDeclaration())
                        {
                            auto summaryIt = summaries.find(callee);
                            const bool hasSummary = summaryIt != summaries.end();
                            const bool hasSummaryEffects =
                                hasSummary && !summaryIt->second.effects.empty();

                            // Treat calls as modeled when either:
                            // - the function is in analysis scope, or
                            // - we have an explicit non-empty summary.
                            // This keeps in-project wrappers precise while still allowing
                            // unknown STL-heavy bodies to remain conservative.
                            if (shouldAnalyze(*callee) || hasSummaryEffects)
                                modeledCall = true;
                        }
                        else if (isStdLibCalleeName(callee->getName()))
                        {
                            // Standard-library functions that are external declarations
                            // (typical for libstdc++ on Linux) are not resource-relevant.
                            // On macOS/libc++ these same functions are often inlined and
                            // would be skipped by the !isDeclaration() path above, so
                            // treating them as modeled here keeps behavior consistent
                            // across platforms.
                            modeledCall = true;
                        }

                        if (!modeledCall && externalSummariesByName)
                        {
                            const auto extIt = externalSummariesByName->find(
                                ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                            if (extIt != externalSummariesByName->end() &&
                                !extIt->second.effects.empty())
                            {
                                modeledCall = true;
                            }
                        }
                    }

                    if (modeledCall)
                        continue;

                    for (unsigned i = 0; i < CB->arg_size(); ++i)
                    {
                        if (argumentMayCarryAddressOfLocal(F, DL, CB->getArgOperand(i), sourceSlot))
                            return true;
                    }
                }
            }
            return false;
        }

        static bool valueClearlyComesFromExternalStorage(const llvm::Value* value,
                                                         const llvm::AllocaInst& slot,
                                                         std::uint64_t offset,
                                                         const llvm::DataLayout& DL,
                                                         unsigned depth = 0)
        {
            if (!value || depth > 8)
                return false;

            const llvm::Value* stripped = value->stripPointerCasts();
            if (llvm::isa<llvm::UndefValue>(stripped) || llvm::isa<llvm::PoisonValue>(stripped))
                return false;
            if (llvm::isa<llvm::ConstantPointerNull>(stripped))
                return false;

            if (valueMayOriginateFromLocalAlloca(stripped, slot, 0))
                return false;

            if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped))
            {
                const llvm::Value* srcCanonical = nullptr;
                std::uint64_t srcOffset = 0;
                if (resolveCanonicalPointerBaseAndOffset(LI->getPointerOperand(), DL, srcCanonical,
                                                         srcOffset))
                {
                    if (srcCanonical == &slot && srcOffset == offset)
                        return false;
                }
                return true;
            }

            if (llvm::isa<llvm::CallBase>(stripped) || llvm::isa<llvm::Argument>(stripped) ||
                llvm::isa<llvm::GlobalValue>(stripped))
            {
                return true;
            }

            if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(stripped))
            {
                for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                {
                    if (valueClearlyComesFromExternalStorage(PN->getIncomingValue(i), slot, offset,
                                                             DL, depth + 1))
                    {
                        return true;
                    }
                }
                return false;
            }

            if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(stripped))
            {
                return valueClearlyComesFromExternalStorage(Sel->getTrueValue(), slot, offset, DL,
                                                            depth + 1) ||
                       valueClearlyComesFromExternalStorage(Sel->getFalseValue(), slot, offset, DL,
                                                            depth + 1);
            }

            return false;
        }

        static bool localStorageHasExplicitExternalStore(const llvm::Function& F,
                                                         const llvm::AllocaInst& slot,
                                                         std::uint64_t slotOffset,
                                                         const llvm::DataLayout& DL)
        {
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                    if (!SI)
                        continue;

                    const llvm::Value* storeCanonical = nullptr;
                    std::uint64_t storeOffset = 0;
                    if (!resolveCanonicalPointerBaseAndOffset(SI->getPointerOperand(), DL,
                                                              storeCanonical, storeOffset))
                    {
                        continue;
                    }
                    if (storeCanonical != &slot || storeOffset != slotOffset)
                        continue;

                    if (valueClearlyComesFromExternalStorage(SI->getValueOperand(), slot,
                                                             slotOffset, DL))
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        static bool pointerContentMayReachReturn(const llvm::Value* ptrStorage)
        {
            if (!ptrStorage || !ptrStorage->getType()->isPointerTy())
                return false;

            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            llvm::SmallVector<const llvm::Value*, 16> worklist;
            worklist.push_back(ptrStorage);

            while (!worklist.empty())
            {
                const llvm::Value* V = worklist.pop_back_val();
                if (!visited.insert(V).second)
                    continue;

                for (const llvm::Use& U : V->uses())
                {
                    const llvm::User* user = U.getUser();
                    if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (LI->getPointerOperand()->stripPointerCasts() == V &&
                            valueMayReachReturn(LI))
                        {
                            return true;
                        }
                        continue;
                    }
                    if (const auto* GEP = llvm::dyn_cast<llvm::GEPOperator>(user))
                    {
                        worklist.push_back(GEP);
                        continue;
                    }
                    if (const auto* BC = llvm::dyn_cast<llvm::BitCastOperator>(user))
                    {
                        worklist.push_back(BC);
                        continue;
                    }
                    if (const auto* PN = llvm::dyn_cast<llvm::PHINode>(user))
                    {
                        worklist.push_back(PN);
                        continue;
                    }
                    if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        worklist.push_back(Sel);
                        continue;
                    }
                }
            }

            return false;
        }

        static void addSummaryEffect(FunctionLifetimeSummary& summary,
                                     std::unordered_set<std::string>& dedup,
                                     const ParamLifetimeEffect& effect)
        {
            const std::string key = encodeSummaryEffectKey(effect);
            if (dedup.insert(key).second)
                summary.effects.push_back(effect);
        }

        static bool functionLifetimeSummaryEquals(const FunctionLifetimeSummary& lhs,
                                                  const FunctionLifetimeSummary& rhs)
        {
            if (lhs.effects.size() != rhs.effects.size())
                return false;
            for (std::size_t i = 0; i < lhs.effects.size(); ++i)
            {
                if (encodeSummaryEffectKey(lhs.effects[i]) !=
                    encodeSummaryEffectKey(rhs.effects[i]))
                    return false;
            }
            return true;
        }

        static const llvm::Value* findUniqueStoredPointerForSlot(const llvm::Function& F,
                                                                 const llvm::DataLayout& DL,
                                                                 const llvm::Value* slotCanonical,
                                                                 std::uint64_t slotOffset)
        {
            const llvm::Value* uniqueStoredPtr = nullptr;
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                    if (!SI)
                        continue;

                    const llvm::Value* stored = SI->getValueOperand()->stripPointerCasts();
                    if (!stored->getType()->isPointerTy())
                        continue;

                    const llvm::Value* storeCanonical = nullptr;
                    std::uint64_t storeOffset = 0;
                    if (!resolveCanonicalPointerBaseAndOffset(SI->getPointerOperand(), DL,
                                                              storeCanonical, storeOffset))
                    {
                        continue;
                    }
                    if (storeCanonical != slotCanonical || storeOffset != slotOffset)
                        continue;

                    if (!uniqueStoredPtr)
                    {
                        uniqueStoredPtr = stored;
                    }
                    else if (uniqueStoredPtr != stored)
                    {
                        return nullptr;
                    }
                }
            }
            return uniqueStoredPtr;
        }

        static StorageKey mapSummaryEffectToCallerStorage(const ParamLifetimeEffect& effect,
                                                          const llvm::CallBase& CB,
                                                          const llvm::Function& callerFunc,
                                                          const MethodClassInfo& callerMethodInfo,
                                                          const llvm::DataLayout& DL,
                                                          bool allowPointerSlotFallback,
                                                          bool* usedPointerSlotFallback)
        {
            if (usedPointerSlotFallback)
                *usedPointerSlotFallback = false;
            if (effect.argIndex >= CB.arg_size())
                return {};

            const llvm::Value* actual = CB.getArgOperand(effect.argIndex);
            if (effect.action == RuleAction::ReleaseArg && !effect.viaPointerSlot &&
                effect.offset == 0)
            {
                StorageKey handleStorage =
                    resolveHandleStorage(actual, callerFunc, DL, callerMethodInfo);
                if (handleStorage.valid())
                    return handleStorage;
            }

            const llvm::Value* canonical = nullptr;
            std::uint64_t baseOffset = 0;
            if (!resolveCanonicalPointerBaseAndOffset(actual, DL, canonical, baseOffset))
                return {};
            if (effect.offset > std::numeric_limits<std::uint64_t>::max() - baseOffset)
                return {};

            const std::uint64_t mappedOffset = baseOffset + effect.offset;
            if (!effect.viaPointerSlot)
            {
                return buildStorageKeyFromCanonical(canonical, mappedOffset, callerFunc,
                                                    callerMethodInfo);
            }

            const llvm::Value* storedPtr =
                findUniqueStoredPointerForSlot(callerFunc, DL, canonical, mappedOffset);
            if (storedPtr)
            {
                StorageKey pointeeStorage =
                    resolvePointerStorage(storedPtr, callerFunc, DL, callerMethodInfo);
                if (pointeeStorage.valid())
                    return pointeeStorage;
            }

            if (!allowPointerSlotFallback)
                return {};

            if (usedPointerSlotFallback)
                *usedPointerSlotFallback = true;
            return buildStorageKeyFromCanonical(canonical, mappedOffset, callerFunc,
                                                callerMethodInfo);
        }

        static FunctionLifetimeSummary buildFunctionLifetimeSummary(
            const llvm::Function& F, const ResourceModel& model,
            const std::unordered_map<const llvm::Function*, FunctionLifetimeSummary>& summaries,
            const std::unordered_map<std::string, FunctionLifetimeSummary>* externalSummariesByName,
            const llvm::DataLayout& DL,
            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
        {
            FunctionLifetimeSummary out;
            std::unordered_set<std::string> dedup;
            const MethodClassInfo methodInfo = describeMethodClass(F);

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;

                    const llvm::Function* callee = resolveDirectCallee(*CB);
                    if (!callee)
                        continue;

                    bool matchedDirectRule = false;
                    for (const ResourceRule& rule : model.rules)
                    {
                        if (!ruleMatchesFunction(rule, *callee))
                            continue;
                        matchedDirectRule = true;

                        if (rule.action == RuleAction::AcquireOut)
                        {
                            if (rule.argIndex >= CB->arg_size())
                                continue;
                            const llvm::Value* outPtr = CB->getArgOperand(rule.argIndex);
                            StorageKey storage = resolvePointerStorage(outPtr, F, DL, methodInfo);
                            bool mappedEffect = false;
                            if (summaryStorageScopeAllowed(storage) && storage.argumentIndex >= 0)
                            {
                                ParamLifetimeEffect effect;
                                effect.action = RuleAction::AcquireOut;
                                effect.argIndex = static_cast<unsigned>(storage.argumentIndex);
                                effect.offset = storage.offset;
                                effect.viaPointerSlot = false;
                                effect.resourceKind = rule.resourceKind;
                                addSummaryEffect(out, dedup, effect);
                                mappedEffect = true;
                            }

                            if (!mappedEffect)
                            {
                                const auto* LI =
                                    llvm::dyn_cast<llvm::LoadInst>(outPtr->stripPointerCasts());
                                if (LI)
                                {
                                    StorageKey slotStorage = resolvePointerStorage(
                                        LI->getPointerOperand(), F, DL, methodInfo);
                                    if (summaryStorageScopeAllowed(slotStorage) &&
                                        slotStorage.argumentIndex >= 0)
                                    {
                                        ParamLifetimeEffect effect;
                                        effect.action = RuleAction::AcquireOut;
                                        effect.argIndex =
                                            static_cast<unsigned>(slotStorage.argumentIndex);
                                        effect.offset = slotStorage.offset;
                                        effect.viaPointerSlot = true;
                                        effect.resourceKind = rule.resourceKind;
                                        addSummaryEffect(out, dedup, effect);
                                        mappedEffect = true;
                                    }
                                }
                            }

                            // Wrapper pattern: acquire into local out-slot then return the handle.
                            if (pointerContentMayReachReturn(outPtr))
                            {
                                ParamLifetimeEffect effect;
                                effect.action = RuleAction::AcquireRet;
                                effect.argIndex = 0;
                                effect.offset = 0;
                                effect.viaPointerSlot = false;
                                effect.resourceKind = rule.resourceKind;
                                addSummaryEffect(out, dedup, effect);
                            }
                            continue;
                        }

                        if (rule.action == RuleAction::ReleaseArg)
                        {
                            if (rule.argIndex >= CB->arg_size())
                                continue;
                            const llvm::Value* handleArg = CB->getArgOperand(rule.argIndex);
                            StorageKey storage = resolveHandleStorage(handleArg, F, DL, methodInfo);
                            if (!summaryStorageScopeAllowed(storage) || storage.argumentIndex < 0)
                                continue;

                            ParamLifetimeEffect effect;
                            effect.action = RuleAction::ReleaseArg;
                            effect.argIndex = static_cast<unsigned>(storage.argumentIndex);
                            effect.offset = storage.offset;
                            effect.viaPointerSlot = false;
                            effect.resourceKind = rule.resourceKind;
                            addSummaryEffect(out, dedup, effect);
                            continue;
                        }

                        if (rule.action == RuleAction::AcquireRet)
                        {
                            if (CB->getType()->isVoidTy())
                                continue;
                            if (!valueMayReachReturn(CB))
                                continue;

                            ParamLifetimeEffect effect;
                            effect.action = RuleAction::AcquireRet;
                            effect.argIndex = 0;
                            effect.offset = 0;
                            effect.viaPointerSlot = false;
                            effect.resourceKind = rule.resourceKind;
                            addSummaryEffect(out, dedup, effect);
                            continue;
                        }
                    }

                    if (matchedDirectRule)
                        continue;
                    if (shouldIgnoreStdLibSummaryPropagation(*callee))
                        continue;
                    const FunctionLifetimeSummary* calleeSummary = nullptr;
                    if (callee->isDeclaration())
                    {
                        // Standard-library external declarations are not resource-relevant.
                        // On macOS/libc++ they would be inlined and skipped entirely;
                        // skip them here as well to keep behavior consistent across
                        // stdlib implementations.
                        if (!isStdLibCalleeName(callee->getName()) && externalSummariesByName)
                        {
                            const auto it = externalSummariesByName->find(
                                ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                            if (it != externalSummariesByName->end())
                                calleeSummary = &it->second;
                        }
                    }
                    else
                    {
                        const MethodClassInfo calleeMethodInfo = describeMethodClass(*callee);
                        if (!calleeMethodInfo.isCtor && !calleeMethodInfo.isDtor)
                        {
                            const auto summaryIt = summaries.find(callee);
                            if (summaryIt != summaries.end())
                                calleeSummary = &summaryIt->second;
                        }

                        if (!calleeSummary && externalSummariesByName)
                        {
                            const auto it = externalSummariesByName->find(
                                ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                            if (it != externalSummariesByName->end())
                                calleeSummary = &it->second;
                        }
                    }

                    if (!calleeSummary)
                        continue;

                    for (const ParamLifetimeEffect& calleeEffect : calleeSummary->effects)
                    {
                        if (calleeEffect.action == RuleAction::AcquireRet)
                        {
                            if (CB->getType()->isVoidTy())
                                continue;

                            std::vector<const llvm::Value*> storeDests;
                            collectStoredDestinations(CB, storeDests);
                            for (const llvm::Value* destPtr : storeDests)
                            {
                                StorageKey storage =
                                    resolvePointerStorage(destPtr, F, DL, methodInfo);
                                if (!summaryStorageScopeAllowed(storage) ||
                                    storage.argumentIndex < 0)
                                {
                                    continue;
                                }

                                ParamLifetimeEffect mappedEffect;
                                mappedEffect.action = RuleAction::AcquireOut;
                                mappedEffect.argIndex =
                                    static_cast<unsigned>(storage.argumentIndex);
                                mappedEffect.offset = storage.offset;
                                mappedEffect.viaPointerSlot = false;
                                mappedEffect.resourceKind = calleeEffect.resourceKind;
                                addSummaryEffect(out, dedup, mappedEffect);
                            }

                            if (valueMayReachReturn(CB))
                            {
                                ParamLifetimeEffect returnEffect;
                                returnEffect.action = RuleAction::AcquireRet;
                                returnEffect.argIndex = 0;
                                returnEffect.offset = 0;
                                returnEffect.viaPointerSlot = false;
                                returnEffect.resourceKind = calleeEffect.resourceKind;
                                addSummaryEffect(out, dedup, returnEffect);
                            }
                            continue;
                        }

                        bool usedFallback = false;
                        StorageKey mappedStorage = mapSummaryEffectToCallerStorage(
                            calleeEffect, *CB, F, methodInfo, DL, true, &usedFallback);
                        if (!mappedStorage.valid())
                            continue;
                        if (!summaryStorageScopeAllowed(mappedStorage) ||
                            mappedStorage.argumentIndex < 0)
                        {
                            continue;
                        }

                        ParamLifetimeEffect mappedEffect;
                        mappedEffect.action = calleeEffect.action;
                        mappedEffect.argIndex = static_cast<unsigned>(mappedStorage.argumentIndex);
                        mappedEffect.offset = mappedStorage.offset;
                        mappedEffect.viaPointerSlot = usedFallback;
                        mappedEffect.resourceKind = calleeEffect.resourceKind;
                        addSummaryEffect(out, dedup, mappedEffect);
                    }
                }
            }

            std::sort(out.effects.begin(), out.effects.end(),
                      [](const ParamLifetimeEffect& lhs, const ParamLifetimeEffect& rhs)
                      { return encodeSummaryEffectKey(lhs) < encodeSummaryEffectKey(rhs); });
            return out;
        }

        static const llvm::Function* resolveDirectCallee(const llvm::CallBase& CB)
        {
            if (const llvm::Function* callee = CB.getCalledFunction())
                return callee;
            const llvm::Value* called = CB.getCalledOperand();
            if (!called)
                return nullptr;
            called = called->stripPointerCasts();
            return llvm::dyn_cast<llvm::Function>(called);
        }

        static const llvm::Instruction* firstInstructionAnchor(const llvm::Function& F)
        {
            const llvm::Instruction* first = nullptr;
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    if (!first)
                        first = &I;
                    if (I.getDebugLoc())
                        return &I;
                }
            }
            return first;
        }

        static std::unordered_map<const llvm::Function*, FunctionLifetimeSummary>
        computeFunctionLifetimeSummaries(
            llvm::Module& mod, const ResourceModel& model,
            const std::function<bool(const llvm::Function&)>& shouldAnalyze,
            const std::unordered_map<std::string, FunctionLifetimeSummary>* externalSummariesByName)
        {
            const llvm::DataLayout& DL = mod.getDataLayout();
            std::unordered_map<const llvm::Function*, FunctionLifetimeSummary> functionSummaries;
            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (shouldIgnoreStdLibSummaryPropagation(F))
                    continue;
                functionSummaries[&F] = {};
            }

            bool changed = true;
            for (unsigned iter = 0; iter < 8 && changed; ++iter)
            {
                changed = false;
                for (llvm::Function& F : mod)
                {
                    if (F.isDeclaration())
                        continue;
                    if (shouldIgnoreStdLibSummaryPropagation(F))
                        continue;
                    FunctionLifetimeSummary nextSummary = buildFunctionLifetimeSummary(
                        F, model, functionSummaries, externalSummariesByName, DL, shouldAnalyze);
                    FunctionLifetimeSummary& currentSummary = functionSummaries[&F];
                    if (!functionLifetimeSummaryEquals(currentSummary, nextSummary))
                    {
                        currentSummary = std::move(nextSummary);
                        changed = true;
                    }
                }
            }
            return functionSummaries;
        }
    } // namespace

    ResourceSummaryIndex buildResourceLifetimeSummaryIndex(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
        const std::string& modelPath, const ResourceSummaryIndex* externalSummaries)
    {
        ResourceSummaryIndex index;
        if (modelPath.empty())
            return index;

        ResourceModel model;
        std::string parseError;
        if (!parseResourceModel(modelPath, model, parseError) || model.rules.empty())
        {
            if (!parseError.empty())
                std::cerr << "Resource model load error: " << parseError << "\n";
            return index;
        }

        const auto externalMap = importExternalSummaryMap(externalSummaries);
        const auto summaries = computeFunctionLifetimeSummaries(
            mod, model, shouldAnalyze, externalMap.empty() ? nullptr : &externalMap);
        return exportSummaryIndexForModule(mod, shouldAnalyze, summaries);
    }

    bool mergeResourceSummaryIndex(ResourceSummaryIndex& dst, const ResourceSummaryIndex& src)
    {
        bool changed = false;
        for (const auto& entry : src.functions)
        {
            auto it = dst.functions.find(entry.first);
            if (it == dst.functions.end())
            {
                dst.functions.emplace(entry.first, entry.second);
                changed = true;
                continue;
            }

            std::unordered_set<std::string> existingKeys;
            existingKeys.reserve(it->second.effects.size());
            for (const ResourceSummaryEffect& effect : it->second.effects)
            {
                ParamLifetimeEffect tmp;
                tmp.action = fromPublicSummaryAction(effect.action);
                tmp.argIndex = effect.argIndex;
                tmp.offset = effect.offset;
                tmp.viaPointerSlot = effect.viaPointerSlot;
                tmp.resourceKind = effect.resourceKind;
                existingKeys.insert(encodeSummaryEffectKey(tmp));
            }

            for (const ResourceSummaryEffect& effect : entry.second.effects)
            {
                ParamLifetimeEffect tmp;
                tmp.action = fromPublicSummaryAction(effect.action);
                tmp.argIndex = effect.argIndex;
                tmp.offset = effect.offset;
                tmp.viaPointerSlot = effect.viaPointerSlot;
                tmp.resourceKind = effect.resourceKind;
                const std::string key = encodeSummaryEffectKey(tmp);
                if (existingKeys.insert(key).second)
                {
                    it->second.effects.push_back(effect);
                    changed = true;
                }
            }
        }
        return changed;
    }

    bool resourceSummaryIndexEquals(const ResourceSummaryIndex& lhs,
                                    const ResourceSummaryIndex& rhs)
    {
        if (lhs.functions.size() != rhs.functions.size())
            return false;
        for (const auto& entry : lhs.functions)
        {
            auto rhsIt = rhs.functions.find(entry.first);
            if (rhsIt == rhs.functions.end())
                return false;

            std::vector<std::string> leftKeys;
            std::vector<std::string> rightKeys;
            leftKeys.reserve(entry.second.effects.size());
            rightKeys.reserve(rhsIt->second.effects.size());

            for (const ResourceSummaryEffect& effect : entry.second.effects)
            {
                ParamLifetimeEffect tmp;
                tmp.action = fromPublicSummaryAction(effect.action);
                tmp.argIndex = effect.argIndex;
                tmp.offset = effect.offset;
                tmp.viaPointerSlot = effect.viaPointerSlot;
                tmp.resourceKind = effect.resourceKind;
                leftKeys.push_back(encodeSummaryEffectKey(tmp));
            }
            for (const ResourceSummaryEffect& effect : rhsIt->second.effects)
            {
                ParamLifetimeEffect tmp;
                tmp.action = fromPublicSummaryAction(effect.action);
                tmp.argIndex = effect.argIndex;
                tmp.offset = effect.offset;
                tmp.viaPointerSlot = effect.viaPointerSlot;
                tmp.resourceKind = effect.resourceKind;
                rightKeys.push_back(encodeSummaryEffectKey(tmp));
            }

            std::sort(leftKeys.begin(), leftKeys.end());
            std::sort(rightKeys.begin(), rightKeys.end());
            if (leftKeys != rightKeys)
                return false;
        }
        return true;
    }

    std::vector<ResourceLifetimeIssue> analyzeResourceLifetime(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
        const std::string& modelPath, const ResourceSummaryIndex* externalSummaries)
    {
        std::vector<ResourceLifetimeIssue> issues;
        if (modelPath.empty())
            return issues;

        ResourceModel model;
        std::string parseError;
        if (!parseResourceModel(modelPath, model, parseError) || model.rules.empty())
        {
            if (!parseError.empty())
                std::cerr << "Resource model load error: " << parseError << "\n";
            return issues;
        }

        const llvm::DataLayout& DL = mod.getDataLayout();
        const auto externalMap = importExternalSummaryMap(externalSummaries);
        std::unordered_map<const llvm::Function*, FunctionLifetimeSummary> functionSummaries =
            computeFunctionLifetimeSummaries(mod, model, shouldAnalyze,
                                             externalMap.empty() ? nullptr : &externalMap);

        std::unordered_map<std::string, ClassLifecycleSummary> classSummaries;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;

            const MethodClassInfo methodInfo = describeMethodClass(F);
            std::unordered_map<std::string, LocalHandleState> localStates;
            std::unordered_map<const llvm::AllocaInst*, bool> unknownAcquireEscapeCache;
            std::unordered_set<std::string> interprocUncertaintyReported;
            auto trackAcquire = [&](const StorageKey& storage, const std::string& resourceKind,
                                    const llvm::Instruction* anchorInst)
            {
                if (!storage.valid())
                    return;

                const std::string stateKey =
                    storage.key + "|" + resourceKind + "|" + F.getName().str();
                LocalHandleState& state = localStates[stateKey];
                state.storage = storage;
                state.resourceKind = resourceKind;
                state.funcName = F.getName().str();
                state.acquires += 1;
                state.ownership = OwnershipState::Owned;
                if (!state.firstAcquireInst)
                    state.firstAcquireInst = anchorInst;

                if (methodInfo.isCtor && storage.scope == StorageScope::ThisField &&
                    !storage.className.empty())
                {
                    ClassLifecycleSummary& summary = classSummaries[storage.className];
                    summary.className = storage.className;
                    const std::string fieldKey = storage.key + "|" + resourceKind;
                    if (summary.ctorAcquires.find(fieldKey) == summary.ctorAcquires.end())
                    {
                        summary.ctorAcquires[fieldKey] = {storage.className, storage.displayName,
                                                          resourceKind, F.getName().str(),
                                                          anchorInst};
                    }
                }
            };

            auto trackRelease = [&](const StorageKey& storage, const std::string& resourceKind,
                                    const llvm::Instruction* anchorInst, bool fromSummary)
            {
                if (!storage.valid())
                    return;

                if (methodInfo.isDtor && storage.scope == StorageScope::ThisField &&
                    !storage.className.empty())
                {
                    ClassLifecycleSummary& summary = classSummaries[storage.className];
                    summary.className = storage.className;
                    summary.dtorReleases.insert(storage.key + "|" + resourceKind);
                    if (summary.dtorAnchor == nullptr)
                    {
                        summary.dtorAnchor = anchorInst;
                        summary.dtorFuncName = F.getName().str();
                    }
                }
                else if (methodInfo.isLifecycleReleaseLike &&
                         storage.scope == StorageScope::ThisField && !storage.className.empty())
                {
                    ClassLifecycleSummary& summary = classSummaries[storage.className];
                    summary.className = storage.className;
                    summary.lifecycleReleases.insert(storage.key + "|" + resourceKind);
                }

                if (storage.scope != StorageScope::Local)
                    return;

                const std::string stateKey =
                    storage.key + "|" + resourceKind + "|" + F.getName().str();
                LocalHandleState& state = localStates[stateKey];
                if (state.storage.scope == StorageScope::Unknown)
                {
                    state.storage = storage;
                    state.resourceKind = resourceKind;
                    state.funcName = F.getName().str();
                }

                state.releases += 1;
                if (state.releases > state.acquires)
                {
                    if (state.acquires == 0 && state.storage.localAlloca)
                    {
                        // Parameter shadow slots often appear as local allocas under optnone.
                        // If the slot is initialized from a function argument and has no local
                        // acquires tracked, treat release as forwarding ownership, not double release.
                        if (resolveAllocaArgumentShadow(*state.storage.localAlloca, false) !=
                            nullptr)
                            return;

                        // Summary-originated releases are conservative by nature: callee-local
                        // acquisitions may be hidden behind unknown/external calls.
                        if (fromSummary)
                        {
                            if (interprocUncertaintyReported.insert(stateKey).second)
                            {
                                coretrace::log(
                                    coretrace::Level::Info,
                                    "[DEBUG-INTERPROC] PATH=fromSummary func={} handle={}\n",
                                    F.getName().str(), storage.displayName);
                                ResourceLifetimeIssue issue;
                                issue.funcName = F.getName().str();
                                issue.resourceKind = resourceKind;
                                issue.handleName = storage.displayName.empty()
                                                       ? std::string("<unknown>")
                                                       : storage.displayName;
                                issue.inst = anchorInst;
                                issue.kind = ResourceLifetimeIssueKind::IncompleteInterproc;
                                issues.push_back(std::move(issue));
                            }
                            state.ownership = OwnershipState::Unknown;
                            return;
                        }

                        if (localStorageHasExplicitExternalStore(F, *state.storage.localAlloca,
                                                                 state.storage.offset, DL))
                        {
                            if (interprocUncertaintyReported.insert(stateKey).second)
                            {
                                coretrace::log(
                                    coretrace::Level::Info,
                                    "[DEBUG-INTERPROC] PATH=externalStore func={} handle={}\n",
                                    F.getName().str(), storage.displayName);
                                ResourceLifetimeIssue issue;
                                issue.funcName = F.getName().str();
                                issue.resourceKind = resourceKind;
                                issue.handleName = storage.displayName.empty()
                                                       ? std::string("<unknown>")
                                                       : storage.displayName;
                                issue.inst = anchorInst;
                                issue.kind = ResourceLifetimeIssueKind::IncompleteInterproc;
                                issues.push_back(std::move(issue));
                            }
                            state.ownership = OwnershipState::Unknown;
                            return;
                        }

                        auto cacheIt = unknownAcquireEscapeCache.find(state.storage.localAlloca);
                        if (cacheIt == unknownAcquireEscapeCache.end())
                        {
                            const bool mayEscapeToUnknownAcquire =
                                localAddressEscapesToUnmodeledCall(
                                    F, *state.storage.localAlloca, model, functionSummaries,
                                    externalMap.empty() ? nullptr : &externalMap, DL,
                                    shouldAnalyze);
                            cacheIt =
                                unknownAcquireEscapeCache
                                    .emplace(state.storage.localAlloca, mayEscapeToUnknownAcquire)
                                    .first;
                        }

                        // If address of this local may be handed to an unmodeled call, we cannot
                        // prove absence of acquisition. Keep diagnostics conservative and avoid a
                        // hard double-release error in this case.
                        if (cacheIt->second)
                        {
                            if (interprocUncertaintyReported.insert(stateKey).second)
                            {
                                coretrace::log(
                                    coretrace::Level::Info,
                                    "[DEBUG-INTERPROC] PATH=escapeUnmodeled func={} handle={}\n",
                                    F.getName().str(), storage.displayName);
                                ResourceLifetimeIssue issue;
                                issue.funcName = F.getName().str();
                                issue.resourceKind = resourceKind;
                                issue.handleName = storage.displayName.empty()
                                                       ? std::string("<unknown>")
                                                       : storage.displayName;
                                issue.inst = anchorInst;
                                issue.kind = ResourceLifetimeIssueKind::IncompleteInterproc;
                                issues.push_back(std::move(issue));
                            }
                            state.ownership = OwnershipState::Unknown;
                            return;
                        }
                    }

                    ResourceLifetimeIssue issue;
                    issue.funcName = F.getName().str();
                    issue.resourceKind = resourceKind;
                    issue.handleName = storage.displayName.empty() ? std::string("<unknown>")
                                                                   : storage.displayName;
                    issue.inst = anchorInst;
                    issue.kind = ResourceLifetimeIssueKind::DoubleRelease;
                    issues.push_back(std::move(issue));
                }
                else if (state.releases == state.acquires)
                {
                    state.ownership = OwnershipState::Released;
                }
                else
                {
                    state.ownership = OwnershipState::Owned;
                }
            };

            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {
                    auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;

                    const llvm::Function* callee = resolveDirectCallee(*CB);
                    if (!callee)
                        continue;

                    bool matchedDirectRule = false;
                    for (const ResourceRule& rule : model.rules)
                    {
                        if (!ruleMatchesFunction(rule, *callee))
                            continue;
                        matchedDirectRule = true;

                        switch (rule.action)
                        {
                        case RuleAction::AcquireOut:
                        {
                            if (rule.argIndex >= CB->arg_size())
                                break;
                            const llvm::Value* outPtr = CB->getArgOperand(rule.argIndex);
                            StorageKey storage = resolvePointerStorage(outPtr, F, DL, methodInfo);
                            trackAcquire(storage, rule.resourceKind, &I);
                            break;
                        }
                        case RuleAction::AcquireRet:
                        {
                            if (CB->getType()->isVoidTy())
                                break;
                            std::vector<const llvm::Value*> storeDests;
                            collectStoredDestinations(CB, storeDests);
                            for (const llvm::Value* destPtr : storeDests)
                            {
                                StorageKey storage =
                                    resolvePointerStorage(destPtr, F, DL, methodInfo);
                                trackAcquire(storage, rule.resourceKind, &I);
                            }
                            break;
                        }
                        case RuleAction::ReleaseArg:
                        {
                            if (rule.argIndex >= CB->arg_size())
                                break;
                            const llvm::Value* handleArg = CB->getArgOperand(rule.argIndex);
                            StorageKey storage = resolveHandleStorage(handleArg, F, DL, methodInfo);
                            trackRelease(storage, rule.resourceKind, &I, false);
                            break;
                        }
                        }
                    }

                    if (matchedDirectRule)
                        continue;
                    if (shouldIgnoreStdLibSummaryPropagation(*callee))
                        continue;
                    const FunctionLifetimeSummary* calleeSummary = nullptr;
                    if (callee->isDeclaration())
                    {
                        if (!isStdLibCalleeName(callee->getName()))
                        {
                            const auto extIt = externalMap.find(
                                ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                            if (extIt != externalMap.end())
                                calleeSummary = &extIt->second;
                        }
                    }
                    else
                    {
                        const MethodClassInfo calleeMethodInfo = describeMethodClass(*callee);
                        if (!calleeMethodInfo.isCtor && !calleeMethodInfo.isDtor)
                        {
                            const auto summaryIt = functionSummaries.find(callee);
                            if (summaryIt != functionSummaries.end())
                                calleeSummary = &summaryIt->second;
                        }

                        if (!calleeSummary)
                        {
                            const auto extIt = externalMap.find(
                                ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                            if (extIt != externalMap.end())
                                calleeSummary = &extIt->second;
                        }
                    }

                    if (!calleeSummary)
                        continue;

                    for (const ParamLifetimeEffect& effect : calleeSummary->effects)
                    {
                        if (effect.action == RuleAction::AcquireRet)
                        {
                            if (CB->getType()->isVoidTy())
                                continue;
                            std::vector<const llvm::Value*> storeDests;
                            collectStoredDestinations(CB, storeDests);
                            for (const llvm::Value* destPtr : storeDests)
                            {
                                StorageKey storage =
                                    resolvePointerStorage(destPtr, F, DL, methodInfo);
                                if (!storage.valid())
                                    continue;
                                trackAcquire(storage, effect.resourceKind, &I);
                            }
                            continue;
                        }

                        StorageKey storage = mapSummaryEffectToCallerStorage(
                            effect, *CB, F, methodInfo, DL, false, nullptr);
                        if (!storage.valid())
                            continue;

                        if (effect.action == RuleAction::AcquireOut)
                        {
                            trackAcquire(storage, effect.resourceKind, &I);
                        }
                        else if (effect.action == RuleAction::ReleaseArg)
                        {
                            trackRelease(storage, effect.resourceKind, &I, true);
                        }
                    }
                }
            }

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(&I);
                    if (!RI)
                        continue;
                    const llvm::Value* retVal = RI->getReturnValue();
                    if (!retVal)
                        continue;

                    StorageKey storage = resolveHandleStorage(retVal, F, DL, methodInfo);
                    for (auto& entry : localStates)
                    {
                        LocalHandleState& state = entry.second;
                        if (state.storage.scope != StorageScope::Local)
                            continue;

                        if (storage.valid() && storage.scope == StorageScope::Local &&
                            (state.storage.key == storage.key ||
                             (state.storage.localAlloca && storage.localAlloca &&
                              state.storage.localAlloca == storage.localAlloca)))
                        {
                            state.escapesViaReturn = true;
                            state.ownership = OwnershipState::Escaped;
                            continue;
                        }

                        // Conservative fallback for optnone-style IR:
                        // return (load %retvalSlot) where %retvalSlot receives the
                        // tracked local handle through one or more local stores.
                        const auto* RIload =
                            llvm::dyn_cast<llvm::LoadInst>(retVal->stripPointerCasts());
                        const auto* retSlot =
                            RIload ? llvm::dyn_cast<llvm::AllocaInst>(
                                         RIload->getPointerOperand()->stripPointerCasts())
                                   : nullptr;
                        if (retSlot && state.storage.localAlloca &&
                            allocaMayStoreValueFromLocalAlloca(*retSlot, *state.storage.localAlloca,
                                                               0))
                        {
                            state.escapesViaReturn = true;
                            state.ownership = OwnershipState::Escaped;
                        }
                    }
                }
            }

            for (const auto& entry : localStates)
            {
                const LocalHandleState& state = entry.second;
                if (state.storage.scope != StorageScope::Local)
                    continue;
                if (state.acquires <= 0)
                    continue;
                if (state.releases >= state.acquires)
                    continue;
                if (state.escapesViaReturn)
                    continue;
                if (state.storage.localAlloca &&
                    localAddressEscapesToUnmodeledCall(
                        F, *state.storage.localAlloca, model, functionSummaries,
                        externalMap.empty() ? nullptr : &externalMap, DL, shouldAnalyze))
                {
                    continue;
                }

                ResourceLifetimeIssue issue;
                issue.funcName = state.funcName;
                issue.resourceKind = state.resourceKind;
                issue.handleName = state.storage.displayName.empty() ? std::string("<unknown>")
                                                                     : state.storage.displayName;
                issue.inst = state.firstAcquireInst;
                issue.kind = ResourceLifetimeIssueKind::MissingRelease;
                issues.push_back(std::move(issue));
            }

            if (methodInfo.isDtor && !methodInfo.className.empty())
            {
                ClassLifecycleSummary& summary = classSummaries[methodInfo.className];
                summary.className = methodInfo.className;
                if (summary.dtorFuncName.empty())
                {
                    summary.dtorFuncName = F.getName().str();
                    summary.dtorAnchor = firstInstructionAnchor(F);
                }
            }
        }

        for (const auto& entry : classSummaries)
        {
            const ClassLifecycleSummary& summary = entry.second;
            if (summary.ctorAcquires.empty())
                continue;

            for (const auto& ctorAcquireEntry : summary.ctorAcquires)
            {
                const std::string& key = ctorAcquireEntry.first;
                const ClassAcquireRecord& acquire = ctorAcquireEntry.second;
                if (summary.dtorReleases.find(key) != summary.dtorReleases.end() ||
                    summary.lifecycleReleases.find(key) != summary.lifecycleReleases.end())
                {
                    continue;
                }

                ResourceLifetimeIssue issue;
                issue.funcName =
                    summary.dtorFuncName.empty() ? acquire.funcName : summary.dtorFuncName;
                issue.resourceKind = acquire.resourceKind;
                issue.handleName = acquire.fieldName;
                issue.className = acquire.className;
                issue.inst = summary.dtorAnchor ? summary.dtorAnchor : acquire.inst;
                issue.kind = ResourceLifetimeIssueKind::MissingDestructorRelease;
                issues.push_back(std::move(issue));
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
