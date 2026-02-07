#include "analysis/InputPipeline.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include "compilerlib/compiler.h"

namespace ctrace::stack::analysis
{
    LanguageType detectFromExtension(const std::string& path)
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

    ModuleLoadResult loadModuleForAnalysis(const std::string& filename,
                                           const AnalysisConfig& config, llvm::LLVMContext& ctx,
                                           llvm::SMDiagnostic& err)
    {
        ModuleLoadResult result;
        result.language = detectLanguageFromFile(filename, ctx);

        if (result.language == LanguageType::Unknown)
        {
            result.error = "Unsupported input file type: " + filename + "\n";
            return result;
        }

        if (result.language != LanguageType::LLVM_IR)
        {
            std::vector<std::string> args;
            args.push_back("-emit-llvm");
            args.push_back("-S");
            args.push_back("-g");
            if (result.language == LanguageType::CXX)
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
                result.error = "Compilation failed:\n" + res.diagnostics + '\n';
                return result;
            }

            if (res.llvmIR.empty())
            {
                result.error = "No LLVM IR produced by compilerlib::compile\n";
                return result;
            }

            auto buffer = llvm::MemoryBuffer::getMemBuffer(res.llvmIR, "in_memory_ll");

            llvm::SMDiagnostic diag;
            result.module = llvm::parseIR(buffer->getMemBufferRef(), diag, ctx);

            if (!result.module)
            {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                diag.print("in_memory_ll", os);
                result.error = "Failed to parse in-memory LLVM IR:\n" + os.str();
                return result;
            }

            return result;
        }

        result.module = llvm::parseIRFile(filename, err, ctx);
        return result;
    }
} // namespace ctrace::stack::analysis
