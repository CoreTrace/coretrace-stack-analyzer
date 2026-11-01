// SPDX-License-Identifier: Apache-2.0
#include "mangle.hpp"

#include <llvm/Demangle/Demangle.h>

namespace ctrace_tools
{
    namespace
    {
        // A symbol, demangled by the C++ runtime's demangler, or by LLVM's when the runtime's
        // predates the symbol's mangling (#240); empty when neither reads it.
        std::string demangled(const std::string& name)
        {
            int status = 0;
            std::unique_ptr<char, void (*)(void*)> out(
                abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status), std::free);
            if (status != 0 || !out)
                out.reset(llvm::itaniumDemangle(name));
            return out ? out.get() : "";
        }
    } // namespace

    bool isMangled(std::string_view name) noexcept
    {
        return name.starts_with("_Z") && !demangled(std::string(name)).empty();
    }

    std::string mangleFunction(const std::string& namespaceName, const std::string& functionName,
                               const std::vector<std::string>& paramTypes)
    {
        std::stringstream mangled;

        // Standard prefix for C++ symbols in the Itanium ABI.
        mangled << "_Z";

        // If a namespace is present, use 'N' and encode the name.
        if (!namespaceName.empty())
        {
            mangled << "N";
            mangled << namespaceName.length() << namespaceName;
        }

        // Add the function name with its length.
        mangled << functionName.length() << functionName;

        // Encode parameter types.
        for (const std::string& param : paramTypes)
        {
            if (param == "int")
            {
                mangled << "i";
            }
            else if (param == "double")
            {
                mangled << "d";
            }
            else if (param == "char")
            {
                mangled << "c";
            }
            else if (param == "std::string")
            {
                mangled << "Ss"; // 'S' for substitution, 's' for std::string
            }
            else if (param == "float")
            {
                mangled << "f";
            }
            else if (param == "bool")
            {
                mangled << "b";
            }
            else if (param == "void")
            {
                mangled << "v";
            }
            else
            {
                // For complex or unknown types, encode as length + name.
                mangled << param.length() << param;
            }
        }

        // Close the namespace with 'E' if used.
        if (!namespaceName.empty())
        {
            mangled << "E";
        }

        return mangled.str();
    }

    std::string demangle(const char* name)
    {
        std::string result = demangled(name);
        return result.empty() ? name : result;
    }

    std::string canonicalizeMangledName(std::string_view name)
    {
        std::string result(name);

        // libc++ (Apple/LLVM): std::__1:: is mangled as St3__1
        for (std::size_t pos = 0; (pos = result.find("St3__1", pos)) != std::string::npos;)
            result.replace(pos, 6, "St");

        // libstdc++ (GCC): std::__cxx11:: is mangled as St7__cxx11
        for (std::size_t pos = 0; (pos = result.find("St7__cxx11", pos)) != std::string::npos;)
            result.replace(pos, 10, "St");

        return result;
    }
}; // namespace ctrace_tools
