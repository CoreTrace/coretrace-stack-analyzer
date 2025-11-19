#include "StackUsageAnalyzer.hpp"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Alignment.h>
#include <llvm/Support/MathExtras.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/MemoryBuffer.h>

#include "compilerlib/compiler.h"

namespace ctrace::stack {

// ============================================================================
// Types internes
// ============================================================================

using CallGraph = std::map<const llvm::Function*, std::vector<const llvm::Function*>>;

enum VisitState { NotVisited = 0, Visiting = 1, Visited = 2 };

// État interne pour la propagation
struct InternalAnalysisState {
    std::map<const llvm::Function*, StackSize> TotalStack;    // stack max, callees inclus
    std::set<const llvm::Function*> RecursiveFuncs;           // fonctions dans au moins un cycle
    std::set<const llvm::Function*> InfiniteRecursionFuncs;   // auto-récursion “infinie”
};

// ============================================================================
// Helpers
// ============================================================================

// Appelle-t-on une autre fonction que soi-même ?
static bool hasNonSelfCall(const llvm::Function &F)
{
    const llvm::Function *Self = &F;

    for (const llvm::BasicBlock &BB : F) {
        for (const llvm::Instruction &I : BB) {

            const llvm::Function *Callee = nullptr;

            if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                Callee = CI->getCalledFunction();
            } else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                Callee = II->getCalledFunction();
            }

            if (Callee && !Callee->isDeclaration() && Callee != Self) {
                return true; // appel vers une autre fonction
            }
        }
    }
    return false;
}

// ============================================================================
//  Analyse locale de la stack (deux variantes)
// ============================================================================

// Mode IR pur : somme des allocas, alignée
static StackSize computeLocalStackIR(llvm::Function &F, const llvm::DataLayout &DL)
{
    StackSize allocSize = 0;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                llvm::Type *ty = alloca->getAllocatedType();
                StackSize count = 1;

                if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize())) {
                    count = CI->getZExtValue();
                } else {
                    llvm::errs() << "Warning: dynamic alloca in function "
                                 << F.getName() << "\n";
                }

                StackSize size = DL.getTypeAllocSize(ty) * count;
                allocSize += size;
            }
        }
    }

    if (allocSize == 0)
        return 0;

    llvm::MaybeAlign MA = DL.getStackAlignment();
    unsigned stackAlign = MA ? MA->value() : 1u;

    if (stackAlign > 1)
        allocSize = llvm::alignTo(allocSize, stackAlign);

    return allocSize;
}

// Mode ABI heuristique : frame minimale + overhead sur calls
static StackSize computeLocalStackABI(llvm::Function &F, const llvm::DataLayout &DL)
{
    StackSize allocSize = 0;

    for (llvm::BasicBlock &BB : F) {
        for (llvm::Instruction &I : BB) {
            if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I)) {
                llvm::Type *ty = alloca->getAllocatedType();
                StackSize count = 1;

                if (auto *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize())) {
                    count = CI->getZExtValue();
                } else {
                    llvm::errs() << "Warning: dynamic alloca in function "
                                 << F.getName() << "\n";
                }

                StackSize size = DL.getTypeAllocSize(ty) * count;
                allocSize += size;
            }
        }
    }

    llvm::MaybeAlign MA = DL.getStackAlignment();
    unsigned stackAlign = MA ? MA->value() : 1u;  // 16 sur beaucoup de cibles

    StackSize frameSize = allocSize;

    if (stackAlign > 1)
        frameSize = llvm::alignTo(frameSize, stackAlign);

    if (!F.isDeclaration() && stackAlign > 1 && frameSize < stackAlign) {
        frameSize = stackAlign;
    }

    if (stackAlign > 1 && hasNonSelfCall(F)) {
        frameSize = llvm::alignTo(frameSize + stackAlign, stackAlign);
    }

    return frameSize;
}

// Wrapper qui sélectionne le mode
static StackSize computeLocalStack(llvm::Function &F,
                                   const llvm::DataLayout &DL,
                                   AnalysisMode mode)
{
    switch (mode) {
        case AnalysisMode::IR:
            return computeLocalStackIR(F, DL);
        case AnalysisMode::ABI:
            return computeLocalStackABI(F, DL);
    }
    return 0;
}

// ============================================================================
//  Construction du graphe d'appels (CallInst / InvokeInst)
// ============================================================================

static CallGraph buildCallGraph(llvm::Module &M)
{
    CallGraph CG;

    for (llvm::Function &F : M) {
        if (F.isDeclaration())
            continue;

        auto &vec = CG[&F];

        for (llvm::BasicBlock &BB : F) {
            for (llvm::Instruction &I : BB) {

                const llvm::Function *Callee = nullptr;

                if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
                    Callee = CI->getCalledFunction();
                } else if (auto *II = llvm::dyn_cast<llvm::InvokeInst>(&I)) {
                    Callee = II->getCalledFunction();
                }

                if (Callee && !Callee->isDeclaration()) {
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

static StackSize dfsComputeStack(
    const llvm::Function *F,
    const CallGraph &CG,
    const std::map<const llvm::Function*, StackSize> &LocalStack,
    std::map<const llvm::Function*, VisitState> &State,
    InternalAnalysisState &Res)
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
            auto itLocal = LocalStack.find(F);
            return (itLocal != LocalStack.end()) ? itLocal->second : 0;
        } else if (itState->second == Visited) {
            auto itTotal = Res.TotalStack.find(F);
            return (itTotal != Res.TotalStack.end()) ? itTotal->second : 0;
        }
    }

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

static InternalAnalysisState computeGlobalStackUsage(
    const CallGraph &CG,
    const std::map<const llvm::Function*, StackSize> &LocalStack)
{
    InternalAnalysisState Res;
    std::map<const llvm::Function*, VisitState> State;

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
//  Détection d’auto-récursion “infinie” (heuristique DominatorTree)
// ============================================================================

static bool detectInfiniteSelfRecursion(llvm::Function &F)
{
    if (F.isDeclaration())
        return false;

    const llvm::Function *Self = &F;

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
                break;
            }
        }
    }

    if (SelfCallBlocks.empty())
        return false;

    llvm::DominatorTree DT(F);

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

                if (!dominatedBySelfCall)
                    return false;
            }
        }
    }

    if (!hasReturn) {
        return true;
    }

    return true;
}

// ============================================================================
//  API publique : analyzeModule / analyzeFile
// ============================================================================

AnalysisResult analyzeModule(llvm::Module &mod,
                             const AnalysisConfig &config)
{
    const llvm::DataLayout &DL = mod.getDataLayout();

    // 1) Stack locale par fonction
    std::map<const llvm::Function*, StackSize> LocalStack;
    for (llvm::Function &F : mod) {
        if (F.isDeclaration())
            continue;
        StackSize sz = computeLocalStack(F, DL, config.mode);
        LocalStack[&F] = sz;
    }

    // 2) Graphe d'appels
    CallGraph CG = buildCallGraph(mod);

    // 3) Propagation + détection de récursivité
    InternalAnalysisState state = computeGlobalStackUsage(CG, LocalStack);

    // 4) Détection d’auto-récursion “infinie” pour les fonctions récursives
    for (llvm::Function &F : mod) {
        if (F.isDeclaration())
            continue;
        const llvm::Function *Fn = &F;
        if (!state.RecursiveFuncs.count(Fn))
            continue;

        if (detectInfiniteSelfRecursion(F)) {
            state.InfiniteRecursionFuncs.insert(Fn);
        }
    }

    // 5) Construction du résultat public
    AnalysisResult result;
    result.config = config;

    for (llvm::Function &F : mod) {
        if (F.isDeclaration())
            continue;

        const llvm::Function *Fn = &F;

        StackSize local = 0;
        StackSize total = 0;

        auto itLocal = LocalStack.find(Fn);
        if (itLocal != LocalStack.end())
            local = itLocal->second;

        auto itTotal = state.TotalStack.find(Fn);
        if (itTotal != state.TotalStack.end())
            total = itTotal->second;

        FunctionResult fr;
        fr.name       = F.getName().str();
        fr.localStack = local;
        fr.maxStack   = total;
        fr.isRecursive = state.RecursiveFuncs.count(Fn) != 0;
        fr.hasInfiniteSelfRecursion = state.InfiniteRecursionFuncs.count(Fn) != 0;
        fr.exceedsLimit = (total > config.stackLimit);

        result.functions.push_back(std::move(fr));
    }

    return result;
}

enum class Language {
    Unknown = 0,
    LLVM_IR,
    C,
    CXX
};

std::string toString(Language lang)
{
    switch (lang) {
        case Language::LLVM_IR: return "LLVM_IR";
        case Language::C:       return "C";
        case Language::CXX:     return "C++";
        default:                return "Unknown";
    }
}


static Language detectFromExtension(const std::string &path)
{
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos)
        return Language::Unknown;

    std::string ext = path.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (ext == "ll")
        return Language::LLVM_IR;

    if (ext == "c")
        return Language::C;

    if (ext == "cpp" || ext == "cc" || ext == "cxx" ||
        ext == "c++" || ext == "cp" || ext == "C")
        return Language::CXX;

    return Language::Unknown;
}

Language detectLanguageFromFile(const std::string &path,
                                llvm::LLVMContext &ctx)
{
    // 1) Tentative LLVM IR (texte)
    {
        llvm::SMDiagnostic diag;
        if (auto mod = llvm::parseIRFile(path, diag, ctx)) {
            return Language::LLVM_IR;
        }
        // si parse échoue, on ignore diag ici (tu peux le log si tu veux)
    }

    // 2) Heuristique extension pour C / C++
    return detectFromExtension(path);
}

AnalysisResult analyzeFile(const std::string &filename,
                           const AnalysisConfig &config,
                           llvm::LLVMContext &ctx,
                           llvm::SMDiagnostic &err)
{

    Language lang = detectLanguageFromFile(filename, ctx);
    std::unique_ptr<llvm::Module> mod;

    std::cout << "Language: " << toString(lang) << "\n";

    if (lang != Language::LLVM_IR)
    {
        std::cout << "Compiling source file to LLVM IR...\n";
        std::vector<std::string> args;
        args.push_back("-emit-llvm");
        args.push_back("-S");
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

    if (lang == Language::LLVM_IR)
    {
        mod = llvm::parseIRFile(filename, err, ctx);
        if (!mod)
        {
            // on laisse err.print au caller si besoin
            return AnalysisResult{config, {}};
        }
    }
    return analyzeModule(*mod, config);
}

} // namespace ctrace::stack
