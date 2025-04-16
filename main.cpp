#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/IR/Constants.h>


int main(int argc, char **argv)
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic err;

    if (argc < 2)
    {
        llvm::errs() << "Usage: stack_usage_analyzer <file.ll>\n";
        return 1;
    }

    std::unique_ptr<llvm::Module> mod = parseIRFile(argv[1], err, context);
    if (!mod)
    {
        err.print("stack_usage_analyzer", llvm::errs());
        return 1;
    }

    for (llvm::Function &F : *mod)
    {
        if (F.isDeclaration()) continue;

        uint64_t totalStack = 0;
        for (llvm::BasicBlock &BB : F)
        {
            for (llvm::Instruction &I : BB)
            {
                if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(&I))
                {
                    llvm::Type *ty = alloca->getAllocatedType();
                    uint64_t count = 1;

                    if (llvm::ConstantInt *CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize()))
                    {
                        count = CI->getZExtValue();
                    }

                    auto &DL = mod->getDataLayout();
                    uint64_t size = DL.getTypeAllocSize(ty) * count;
                    totalStack += size;
                }
            }
        }

        llvm::outs() << F.getName() << ": " << totalStack << " bytes\n";
    }

    return 0;
}
