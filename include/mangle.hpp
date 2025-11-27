#include <string>
#include <vector>
#include <sstream>
#include <iostream>
#include <cxxabi.h>
#include <memory>

namespace ctrace_tools
{
    /**
     * @brief Concept to define types that can be converted to `std::string_view`.
     *
     * The `StringLike` concept ensures that any type passed to functions
     * requiring it can be implicitly converted to `std::string_view`.
     */
    template<typename T>
    concept StringLike = std::convertible_to<T, std::string_view>;
    // TODO: add mangling for windows

    /**
     * @brief Checks if a given name is a mangled C++ symbol.
     *
     * This function determines whether a given name follows the Itanium C++ ABI
     * mangling conventions (e.g., names starting with `_Z`).
     *
     * @tparam T A type satisfying the `StringLike` concept.
     * @param name The name to check for mangling.
     * @return `true` if the name is mangled, `false` otherwise.
     *
     * @note This function uses `abi::__cxa_demangle` to attempt demangling.
     *       If the demangling succeeds, the name is considered mangled.
     * @note This implementation is specific to platforms using the Itanium C++ ABI
     *       (e.g., Linux, macOS). Windows mangling is not yet supported.
     * @note The function is marked `[[nodiscard]]`, meaning the return value
     *       should not be ignored. It is also `noexcept`, indicating that it
     *       does not throw exceptions.
     */
    [[nodiscard]]bool isMangled(StringLike auto name) noexcept
    {
        int status = 0;
        std::string_view sv{name};

        if (sv.length() < 2 || sv.substr(0, 2) != "_Z")
        {
            return false;
        }

        std::unique_ptr<char, void(*)(void*)> demangled(
            abi::__cxa_demangle(sv.data(), nullptr, nullptr, &status),
            std::free
        );
        return status == 0;
    }

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
    [[nodiscard]] std::string mangleFunction(
        const std::string& namespaceName,
        const std::string& functionName,
        const std::vector<std::string>& paramTypes
    );

    [[nodiscard]] std::string demangle(const char *name);
};
