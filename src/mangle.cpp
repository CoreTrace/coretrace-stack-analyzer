#include "mangle.hpp"

namespace ctrace_tools {

    std::string mangleFunction(const std::string& namespaceName,
        const std::string& functionName,
        const std::vector<std::string>& paramTypes)
    {
        std::stringstream mangled;

        // Préfixe standard pour les symboles C++ dans l'Itanium ABI
        mangled << "_Z";

        // Si un namespace est présent, on utilise 'N' et on encode le nom
        if (!namespaceName.empty())
        {
            mangled << "N";
            mangled << namespaceName.length() << namespaceName;
        }

        // Ajouter le nom de la fonction avec sa longueur
        mangled << functionName.length() << functionName;

        // Encoder les types de paramètres
        for (const std::string& param : paramTypes) {
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
                mangled << "Ss"; // 'S' pour substitution, 's' pour std::string
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
            else {
            // Pour les types complexes ou non reconnus, encoder avec longueur + nom
                mangled << param.length() << param;
            }
        }

        // Fermer le namespace avec 'E' si utilisé
        if (!namespaceName.empty())
        {
            mangled << "E";
        }

        return mangled.str();
    }

    std::string demangle(const char *name)
    {
        int status = 0;
        char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);

        std::string result = (status == 0 && demangled)
            ? demangled
            : name;

        free(demangled);

        return result;
    }
};
