#include "mangle.hpp"

namespace ctrace_tools
{

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
        int status = 0;
        char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);

        std::string result = (status == 0 && demangled) ? demangled : name;

        free(demangled);

        return result;
    }
}; // namespace ctrace_tools
