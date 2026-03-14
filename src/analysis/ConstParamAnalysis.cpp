#include "analysis/ConstParamAnalysis.hpp"

#include <cctype>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        struct TypeQualifiers
        {
            std::uint64_t isConst : 1 = false;
            std::uint64_t isVolatile : 1 = false;
            std::uint64_t isRestrict : 1 = false;
            std::uint64_t reservedFlags : 61 = 0;
        };

        struct StrippedDIType
        {
            const llvm::DIType* type = nullptr;
            TypeQualifiers quals;
        };

        struct ParamTypeInfo
        {
            const llvm::DIType* originalType = nullptr;
            const llvm::DIType* pointeeType = nullptr;        // unqualified, typedefs stripped
            const llvm::DIType* pointeeDisplayType = nullptr; // unqualified, typedefs preserved
            std::uint64_t isPointer : 1 = false;
            std::uint64_t isReference : 1 = false;
            std::uint64_t isRvalueReference : 1 = false;
            std::uint64_t pointerConst : 1 = false;
            std::uint64_t pointerVolatile : 1 = false;
            std::uint64_t pointerRestrict : 1 = false;
            std::uint64_t pointeeConst : 1 = false;
            std::uint64_t pointeeVolatile : 1 = false;
            std::uint64_t pointeeRestrict : 1 = false;
            std::uint64_t isDoublePointer : 1 = false;
            std::uint64_t isVoid : 1 = false;
            std::uint64_t isFunctionPointer : 1 = false;
            std::uint64_t reservedFlags : 52 = 0;
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
                if (baseTag == dwarf::DW_TAG_pointer_type ||
                    baseTag == dwarf::DW_TAG_reference_type ||
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

        enum class ParamWriteState : std::uint8_t
        {
            NoWrite = 0,
            Unknown = 1,
            MayWrite = 2
        };

        static ParamWriteState mergeParamWriteState(ParamWriteState lhs, ParamWriteState rhs)
        {
            if (lhs == ParamWriteState::MayWrite || rhs == ParamWriteState::MayWrite)
                return ParamWriteState::MayWrite;
            if (lhs == ParamWriteState::Unknown || rhs == ParamWriteState::Unknown)
                return ParamWriteState::Unknown;
            return ParamWriteState::NoWrite;
        }

        static bool calleeParamIsReadOnly(const llvm::Function* callee, unsigned argIndex)
        {
            if (!callee || argIndex >= callee->arg_size())
                return false;

            const llvm::Argument& param = *callee->getArg(argIndex);
            const ParameterDebugBinding binding = resolveParameterDebugBinding(*callee, param);
            if (!binding.type)
                return false;

            ParamTypeInfo typeInfo;
            if (!buildParamTypeInfo(binding.type, typeInfo))
                return false;

            if (typeInfo.isDoublePointer || typeInfo.isVoid || typeInfo.isFunctionPointer)
                return false;

            if (!typeInfo.isPointer && !typeInfo.isReference)
                return false;

            return typeInfo.pointeeConst;
        }

        static ParamWriteState callArgWriteState(const llvm::CallBase& CB, unsigned argIndex)
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
                return ParamWriteState::Unknown;

            if (auto* MI = dyn_cast<MemIntrinsic>(&CB))
            {
                if (isa<MemSetInst>(MI))
                    return argIndex == 0 ? ParamWriteState::MayWrite : ParamWriteState::NoWrite;
                if (isa<MemTransferInst>(MI))
                    return argIndex == 0 ? ParamWriteState::MayWrite : ParamWriteState::NoWrite;
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
                    return ParamWriteState::NoWrite;
                default:
                    break;
                }
            }

            if (callee->doesNotAccessMemory())
                return ParamWriteState::NoWrite;
            if (callee->onlyReadsMemory())
                return ParamWriteState::NoWrite;

            if (argIndex >= callee->arg_size())
                return ParamWriteState::Unknown; // varargs or unknown

            const AttributeList& attrs = callee->getAttributes();
            if (attrs.hasParamAttr(argIndex, Attribute::ReadOnly) ||
                attrs.hasParamAttr(argIndex, Attribute::ReadNone))
            {
                return ParamWriteState::NoWrite;
            }
            if (attrs.hasParamAttr(argIndex, Attribute::WriteOnly))
                return ParamWriteState::MayWrite;

            if (const auto* calleeArg = callee->getArg(argIndex))
            {
                if (calleeArg->onlyReadsMemory())
                    return ParamWriteState::NoWrite;
            }

            if (calleeParamIsReadOnly(callee, argIndex))
                return ParamWriteState::NoWrite;

            return ParamWriteState::Unknown;
        }

        static ParamWriteState argumentWriteStateFromMetadata(const llvm::Argument& Arg)
        {
            using namespace llvm;

            if (Arg.hasAttribute(Attribute::WriteOnly))
                return ParamWriteState::MayWrite;

            if (Arg.onlyReadsMemory() || Arg.hasAttribute(Attribute::ReadOnly) ||
                Arg.hasAttribute(Attribute::ReadNone))
            {
                return ParamWriteState::NoWrite;
            }

            return ParamWriteState::Unknown;
        }

        static ParamWriteState valueWriteState(const llvm::Value* root, const llvm::Function& F)
        {
            using namespace llvm;
            (void)F;

            SmallPtrSet<const Value*, 32> visited;
            SmallVector<const Value*, 16> worklist;
            worklist.push_back(root);
            ParamWriteState aggregate = ParamWriteState::NoWrite;

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
                            return ParamWriteState::MayWrite;
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
                                aggregate = mergeParamWriteState(aggregate, ParamWriteState::Unknown);
                            }
                        }
                        continue;
                    }

                    if (auto* AI = dyn_cast<AtomicRMWInst>(Usr))
                    {
                        if (AI->getPointerOperand() == V)
                            return ParamWriteState::MayWrite;
                        continue;
                    }

                    if (auto* CX = dyn_cast<AtomicCmpXchgInst>(Usr))
                    {
                        if (CX->getPointerOperand() == V)
                            return ParamWriteState::MayWrite;
                        continue;
                    }

                    if (auto* CB = dyn_cast<CallBase>(Usr))
                    {
                        for (unsigned i = 0; i < CB->arg_size(); ++i)
                        {
                            if (CB->getArgOperand(i) == V)
                            {
                                const ParamWriteState callWriteState = callArgWriteState(*CB, i);
                                if (callWriteState == ParamWriteState::MayWrite)
                                    return ParamWriteState::MayWrite;
                                aggregate = mergeParamWriteState(aggregate, callWriteState);
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
                    {
                        aggregate = mergeParamWriteState(aggregate, ParamWriteState::Unknown);
                        continue;
                    }
                }
            }

            return aggregate;
        }

        static bool isAnonymousTypeName(llvm::StringRef typeName)
        {
            return typeName.empty() || typeName == "<unknown type>" || typeName == "<anonymous type>";
        }

        static bool isLikelyForwardingTypeName(llvm::StringRef typeName)
        {
            if (typeName.empty())
                return false;
            if (typeName.size() == 1)
            {
                const unsigned char c = static_cast<unsigned char>(typeName.front());
                if (std::isalpha(c) && std::isupper(c))
                    return true;
            }
            if (typeName.contains("type-parameter-"))
                return true;
            if (typeName.ends_with("_Tp"))
                return true;
            return false;
        }

        static bool argumentLooksCallable(const llvm::Argument& Arg)
        {
            for (const llvm::Use& use : Arg.uses())
            {
                const auto* CB = llvm::dyn_cast<llvm::CallBase>(use.getUser());
                if (!CB)
                    continue;
                if (CB->isCallee(&use))
                    return true;
                const llvm::Function* callee = CB->getCalledFunction();
                if (!callee)
                    continue;
                if (const llvm::DISubprogram* SP = callee->getSubprogram())
                {
                    if (SP->getName().contains("operator()"))
                        return true;
                }
            }
            return false;
        }

        static bool shouldSuppressConstParamIssue(const llvm::Argument& Arg,
                                                  const ParameterDebugBinding& binding,
                                                  const ParamTypeInfo& typeInfo,
                                                  llvm::StringRef baseTypeName)
        {
            if (binding.isArtificial)
                return true;

            if (typeInfo.isRvalueReference)
            {
                if (isLikelyForwardingTypeName(baseTypeName))
                    return true;
                if (argumentLooksCallable(Arg))
                    return true;
            }

            return false;
        }

        static double computeIssueConfidence(const ParameterDebugBinding& binding,
                                             bool provenReadOnlyByMetadata,
                                             bool anonymousType)
        {
            double confidence = 0.58;
            switch (binding.confidence)
            {
            case ParameterBindingConfidence::High:
                confidence = 0.85;
                break;
            case ParameterBindingConfidence::Medium:
                confidence = 0.72;
                break;
            case ParameterBindingConfidence::Low:
                confidence = 0.58;
                break;
            }
            if (provenReadOnlyByMetadata)
                confidence += 0.07;
            if (anonymousType && confidence > 0.50)
                confidence = 0.50;
            if (confidence > 0.95)
                confidence = 0.95;
            return confidence;
        }

        static bool hasDtorToken(llvm::StringRef symbol, llvm::StringRef token)
        {
            std::size_t pos = symbol.find(token);
            while (pos != llvm::StringRef::npos)
            {
                const std::size_t nextPos = pos + token.size();
                if (nextPos >= symbol.size())
                    return true;

                const char next = symbol[nextPos];
                if (next == 'E' || next == 'B' || next == 'v' || next == 'I')
                    return true;

                pos = symbol.find(token, pos + 1);
            }
            return false;
        }

        static bool isLikelyCppDestructor(const llvm::Function& F)
        {
            const llvm::StringRef symbol = F.getName();
            if (symbol.starts_with("_Z"))
            {
                return hasDtorToken(symbol, "D0") || hasDtorToken(symbol, "D1") ||
                       hasDtorToken(symbol, "D2");
            }

            if (const llvm::DISubprogram* SP = F.getSubprogram())
            {
                if (SP->getName().contains("~"))
                    return true;
            }

            return false;
        }

        static void analyzeConstParamsInFunction(llvm::Function& F,
                                                 std::vector<ConstParamIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;
            if (isLikelyCppDestructor(F))
                return;

            for (Argument& Arg : F.args())
            {
                const ParameterDebugBinding binding = resolveParameterDebugBinding(F, Arg);
                if (!binding.type)
                    continue;

                ParamTypeInfo typeInfo;
                if (!buildParamTypeInfo(binding.type, typeInfo))
                    continue;

                if (!typeInfo.isPointer && !typeInfo.isReference)
                    continue;
                if (typeInfo.isDoublePointer || typeInfo.isVoid || typeInfo.isFunctionPointer)
                    continue;
                if (typeInfo.pointeeConst)
                    continue;

                const ParamWriteState metadataWriteState = argumentWriteStateFromMetadata(Arg);
                if (metadataWriteState == ParamWriteState::MayWrite)
                    continue;
                const bool readOnlyByMetadata = metadataWriteState == ParamWriteState::NoWrite;

                const ParamWriteState writeState =
                    readOnlyByMetadata ? ParamWriteState::NoWrite : valueWriteState(&Arg, F);
                if (writeState != ParamWriteState::NoWrite)
                    continue;

                const std::string paramName = binding.name.empty() ? Arg.getName().str() : binding.name;
                const std::string baseName = formatDITypeName(typeInfo.pointeeDisplayType);
                const bool anonymousType = isAnonymousTypeName(baseName);
                if (shouldSuppressConstParamIssue(Arg, binding, typeInfo, baseName))
                    continue;

                ConstParamIssue issue;
                issue.funcName = F.getName().str();
                issue.binding = binding;
                issue.confidence =
                    computeIssueConfidence(binding, readOnlyByMetadata, anonymousType);
                issue.pointerConstOnly =
                    typeInfo.isPointer && typeInfo.pointerConst && !typeInfo.pointeeConst;
                issue.isReference = typeInfo.isReference;
                issue.isRvalueRef = typeInfo.isRvalueReference;

                issue.currentType =
                    buildTypeString(typeInfo, baseName, false, true, paramName);
                if (typeInfo.isRvalueReference)
                {
                    std::string valuePrefix = buildPointeeQualPrefix(typeInfo, false);
                    std::string constRefPrefix = buildPointeeQualPrefix(typeInfo, true);
                    issue.suggestedType = valuePrefix + baseName + " " + paramName;
                    issue.suggestedTypeAlt = constRefPrefix + baseName + " &" + paramName;
                }
                else
                {
                    issue.suggestedType = buildTypeString(typeInfo, baseName, true, false, paramName);
                }

                out.push_back(std::move(issue));
            }
        }
    } // namespace

    std::vector<ConstParamIssue>
    analyzeConstParams(llvm::Module& mod,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<ConstParamIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeConstParamsInFunction(F, out);
        }

        return out;
    }
} // namespace ctrace::stack::analysis
