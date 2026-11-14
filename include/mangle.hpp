// SPDX-License-Identifier: Apache-2.0
#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <cxxabi.h>
#include <memory>

namespace ctrace_tools
{
    // TODO: add mangling for windows

    /**
     * @brief Checks if a given name is a mangled C++ symbol.
     *
     * This function determines whether a given name follows the Itanium C++ ABI
     * mangling conventions (e.g., names starting with `_Z`).
     *
     * @param name The name to check for mangling.
     * @return `true` if the name is mangled, `false` otherwise.
     *
     * @note The name is mangled if a demangler reads it: the C++ runtime's
     *       (`abi::__cxa_demangle`), or LLVM's when the runtime's predates the
     *       name's mangling, as a C++23 explicit object member's on macOS 14.
     * @note This implementation is specific to platforms using the Itanium C++ ABI
     *       (e.g., Linux, macOS). Windows mangling is not yet supported.
     * @note The function is marked `[[nodiscard]]`, meaning the return value
     *       should not be ignored. It is also `noexcept`, indicating that it
     *       does not throw exceptions.
     */
    [[nodiscard]] bool isMangled(std::string_view name) noexcept;

    /**
     * @brief Generates a mangled name for a function.
     *
     * This function creates a mangled name for a function based on its namespace,
     * name, and parameter types. The mangling follows the Itanium C++ ABI conventions.
     *
     * @param namespaceName The namespace of the function.
     * @param functionName The name of the function.
     * @param paramTypes A vector of strings representing the parameter types.
     * @return A `std::string` containing the mangled name.
     *
     * @note The implementation of this function is not provided in the current file.
     */
    [[nodiscard]] std::string mangleFunction(const std::string& namespaceName,
                                             const std::string& functionName,
                                             const std::vector<std::string>& paramTypes);

    [[nodiscard]] std::string demangle(const char* name);

    /**
     * @brief Canonicalizes a mangled C++ symbol by normalizing standard-library
     *        implementation namespaces.
     *
     * Different C++ standard library implementations encode their internal
     * namespace differently in mangled names:
     *   - libc++ (Apple/LLVM): `std::__1::`  → mangled as `St3__1`
     *   - libstdc++ (GCC):     `std::__cxx11::` → mangled as `St7__cxx11`
     *
     * This function replaces both variants with the bare `St` prefix so that
     * cross-TU summary lookups match regardless of the stdlib used to compile
     * each translation unit.
     *
     * @param name The (possibly mangled) symbol name.
     * @return A new string with stdlib namespace prefixes normalized.
     */
    [[nodiscard]] std::string canonicalizeMangledName(std::string_view name);
}; // namespace ctrace_tools
