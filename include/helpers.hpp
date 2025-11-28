#pragma once

#include <array>
#include <string_view>
#include <type_traits>

namespace ctrace::stack
{

template<typename E>
struct EnumTraits; // pas de définition générique -> erreur si non spécialisé

template<typename E>
concept EnumWithTraits = std::is_enum_v<E> && requires {
    EnumTraits<E>::names;
};

template<EnumWithTraits E>
constexpr std::string_view enumToString(E e) noexcept
{
    using Traits = EnumTraits<E>;
    using U = std::underlying_type_t<E>;

    constexpr auto size = Traits::names.size();
    const auto idx = static_cast<U>(e);

    if (idx < 0 || static_cast<std::size_t>(idx) >= size)
        return "Unknown";

    return Traits::names[static_cast<std::size_t>(idx)];
}

} // namespace ctrace::stack
