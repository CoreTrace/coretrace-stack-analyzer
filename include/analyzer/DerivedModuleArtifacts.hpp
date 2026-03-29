// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ctrace::stack::analyzer
{
    inline constexpr std::uint32_t kDerivedModuleArtifactsSchemaVersion = 1;
    inline constexpr std::string_view kDerivedModuleArtifactsSchemaKey =
        "derived-module-artifacts-v1";

    struct DebugMetadataIndex
    {
        std::uint64_t allDefinedFunctionsWithSubprogram = 0;
        std::uint64_t selectedFunctionsWithSubprogram = 0;
        std::uint64_t distinctSourceFiles = 0;
        std::unordered_map<std::string, std::uint32_t> functionsPerSourceFile;
    };

    struct FunctionSymbolIndex
    {
        std::uint64_t totalDefinedFunctions = 0;
        std::uint64_t distinctMangledNames = 0;
        std::unordered_map<std::string, std::uint32_t> mangledNameFrequency;
    };

    struct TypeFactIndex
    {
        std::uint64_t pointerReturnFunctionCount = 0;
        std::uint64_t aggregateReturnFunctionCount = 0;
        std::uint64_t pointerParameterCount = 0;
        std::uint64_t aggregateParameterCount = 0;
    };

    struct DerivedModuleArtifacts
    {
        std::uint32_t schemaVersion = kDerivedModuleArtifactsSchemaVersion;
        std::uint32_t reservedPadding = 0;
        DebugMetadataIndex debugIndex;
        FunctionSymbolIndex symbolIndex;
        TypeFactIndex typeFacts;

        [[nodiscard]] bool hasCompatibleSchema() const
        {
            return schemaVersion == kDerivedModuleArtifactsSchemaVersion;
        }

        [[nodiscard]] static constexpr std::string_view schemaKey()
        {
            return kDerivedModuleArtifactsSchemaKey;
        }
    };
} // namespace ctrace::stack::analyzer
