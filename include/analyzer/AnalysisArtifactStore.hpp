// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>

namespace ctrace::stack::analyzer
{
    class AnalysisArtifactStore
    {
      public:
        template <typename T, typename... Args> T& emplace(Args&&... args)
        {
            auto artifact = std::make_shared<T>(std::forward<Args>(args)...);
            auto key = std::type_index(typeid(T));
            artifacts_[key] = artifact;
            return *artifact;
        }

        template <typename T> void set(const T& value)
        {
            (void)emplace<T>(value);
        }

        template <typename T> void set(T&& value)
        {
            (void)emplace<T>(std::move(value));
        }

        template <typename T> [[nodiscard]] bool has() const
        {
            return artifacts_.find(std::type_index(typeid(T))) != artifacts_.end();
        }

        template <typename T> [[nodiscard]] T* get()
        {
            auto it = artifacts_.find(std::type_index(typeid(T)));
            if (it == artifacts_.end())
                return nullptr;
            return static_cast<T*>(it->second.get());
        }

        template <typename T> [[nodiscard]] const T* get() const
        {
            auto it = artifacts_.find(std::type_index(typeid(T)));
            if (it == artifacts_.end())
                return nullptr;
            return static_cast<const T*>(it->second.get());
        }

        void clear()
        {
            artifacts_.clear();
        }

      private:
        std::unordered_map<std::type_index, std::shared_ptr<void>> artifacts_;
    };
} // namespace ctrace::stack::analyzer
