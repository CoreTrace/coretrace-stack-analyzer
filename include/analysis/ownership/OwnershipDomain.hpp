// SPDX-License-Identifier: Apache-2.0
// Abstract domain of the resource ownership engine. LLVM-free.
//
// A *resource* is an acquired instance; a *location* is a slot that may hold
// references to resources. The two are tracked separately so that copying a
// handle never loses the obligation attached to the resource it names.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ctrace::stack::analysis::ownership
{
    enum class OwnState : std::uint8_t
    {
        NotOwned = 0,
        Owned = 1,
        Released = 2,
        Escaped = 3
    };

    /// A set of possible ownership states. The empty set is the ⊥ component.
    struct StateSet
    {
        std::uint8_t bits = 0;

        static constexpr StateSet of(OwnState s)
        {
            return StateSet{static_cast<std::uint8_t>(1u << static_cast<unsigned>(s))};
        }
        static constexpr StateSet none()
        {
            return StateSet{0};
        }

        [[nodiscard]] constexpr bool has(OwnState s) const
        {
            return (bits & of(s).bits) != 0;
        }
        [[nodiscard]] constexpr bool empty() const
        {
            return bits == 0;
        }
        [[nodiscard]] constexpr bool isOnly(OwnState s) const
        {
            return bits == of(s).bits;
        }

        constexpr StateSet& operator|=(StateSet o)
        {
            bits = static_cast<std::uint8_t>(bits | o.bits);
            return *this;
        }
        friend constexpr StateSet operator|(StateSet a, StateSet b)
        {
            return a |= b;
        }
        friend constexpr bool operator==(StateSet a, StateSet b)
        {
            return a.bits == b.bits;
        }
    };

    /// Resource ids come in pairs per acquisition site: `2*site` is the latest
    /// instance, `2*site + 1` summarises every earlier one (loops).
    using ResourceId = std::uint32_t;
    using LocationId = std::uint32_t;

    constexpr ResourceId newInstanceOf(std::uint32_t site)
    {
        return 2u * site;
    }
    constexpr ResourceId oldInstancesOf(std::uint32_t site)
    {
        return 2u * site + 1u;
    }

    /// What a location may hold: a sorted set of resources, possibly null,
    /// possibly something the analysis cannot see (address escaped, unknown call).
    struct Contents
    {
        std::vector<ResourceId> resources;
        bool mayNull = false;
        bool mayUnknown = false;
        std::uint8_t reservedPadding[6] = {};

        [[nodiscard]] bool empty() const
        {
            return resources.empty() && !mayNull && !mayUnknown;
        }
        [[nodiscard]] bool isExactly(ResourceId r) const
        {
            return resources.size() == 1 && resources[0] == r && !mayNull && !mayUnknown;
        }
        [[nodiscard]] bool holds(ResourceId r) const
        {
            return std::binary_search(resources.begin(), resources.end(), r);
        }

        void add(ResourceId r)
        {
            const auto it = std::lower_bound(resources.begin(), resources.end(), r);
            if (it == resources.end() || *it != r)
                resources.insert(it, r);
        }
        void remove(ResourceId r)
        {
            const auto it = std::lower_bound(resources.begin(), resources.end(), r);
            if (it != resources.end() && *it == r)
                resources.erase(it);
        }
        void merge(const Contents& o)
        {
            for (const ResourceId r : o.resources)
                add(r);
            mayNull = mayNull || o.mayNull;
            mayUnknown = mayUnknown || o.mayUnknown;
        }

        friend bool operator==(const Contents& a, const Contents& b)
        {
            return a.resources == b.resources && a.mayNull == b.mayNull &&
                   a.mayUnknown == b.mayUnknown;
        }
    };

    /// The abstract state at a program point.
    struct AbstractState
    {
        std::vector<StateSet> resources;
        std::vector<bool> uncertain; // sticky: the resource can only be "insufficient information"
        std::vector<Contents> locations;
        bool reached = false; // false ⇔ ⊥
        std::uint8_t reservedPadding[7] = {};

        static AbstractState bottom(std::size_t resourceCount, std::size_t locationCount)
        {
            AbstractState s;
            s.resources.assign(resourceCount, StateSet::none());
            s.uncertain.assign(resourceCount, false);
            s.locations.assign(locationCount, Contents{});
            return s;
        }

        static AbstractState entry(std::size_t resourceCount, std::size_t locationCount)
        {
            AbstractState s = bottom(resourceCount, locationCount);
            s.reached = true;
            std::fill(s.resources.begin(), s.resources.end(), StateSet::of(OwnState::NotOwned));
            return s;
        }

        void join(const AbstractState& o)
        {
            if (!o.reached)
                return;
            if (!reached)
            {
                *this = o;
                return;
            }
            for (std::size_t i = 0; i < resources.size() && i < o.resources.size(); ++i)
                resources[i] |= o.resources[i];
            for (std::size_t i = 0; i < uncertain.size() && i < o.uncertain.size(); ++i)
                uncertain[i] = uncertain[i] || o.uncertain[i];
            for (std::size_t i = 0; i < locations.size() && i < o.locations.size(); ++i)
                locations[i].merge(o.locations[i]);
        }

        friend bool operator==(const AbstractState& a, const AbstractState& b)
        {
            return a.reached == b.reached && a.resources == b.resources &&
                   a.uncertain == b.uncertain && a.locations == b.locations;
        }
    };
} // namespace ctrace::stack::analysis::ownership
