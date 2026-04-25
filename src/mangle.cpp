// SPDX-License-Identifier: Apache-2.0
#include "mangle.hpp"

namespace ctrace_tools
{
    std::string mangleFunction(const std::string& namespaceName, const std::string& functionName,
                               const std::vector<std::string>& paramTypes)
    {
        std::stringstream mangled;

        mangled << "_Z";

        if (!namespaceName.empty())
        {
            mangled << "N";
            mangled << namespaceName.length() << namespaceName;
        }

        mangled << functionName.length() << functionName;

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
                mangled << "Ss";
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
                mangled << param.length() << param;
            }
        }

        if (!namespaceName.empty())
        {
            mangled << "E";
        }

        return mangled.str();
    }

    std::string demangle(const char* name)
    {
        if (name == nullptr)
        {
            return {};
        }

        return llvm::demangle(name);
    }

    std::string canonicalizeMangledName(std::string_view name)
    {
        std::string result(name);

        for (std::size_t pos = 0; (pos = result.find("St3__1", pos)) != std::string::npos;)
        {
            result.replace(pos, 6, "St");
        }

        for (std::size_t pos = 0; (pos = result.find("St7__cxx11", pos)) != std::string::npos;)
        {
            result.replace(pos, 10, "St");
        }

        return result;
    }
} // namespace ctrace_tools
