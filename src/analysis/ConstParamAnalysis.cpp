#include "analysis/ConstParamAnalysis.hpp"

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

        static void analyzeConstParamsInFunction(llvm::Function& F,
                                                 std::vector<ConstParamIssue>& out)
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
                issue.currentType =
                    buildTypeString(typeInfo, baseName, false, true, issue.paramName);
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
