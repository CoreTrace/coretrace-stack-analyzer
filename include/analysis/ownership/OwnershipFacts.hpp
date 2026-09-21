// SPDX-License-Identifier: Apache-2.0
// The LLVM-free description of a function consumed by the ownership engine:
// blocks of typed events, edges (with their own events for effects that hold
// on one edge only), locations, and the transformer summaries of callees.
#pragma once

#include "analysis/ownership/OwnershipDomain.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ctrace::stack::analysis::ownership
{
    enum class Certainty : std::uint8_t
    {
        Guaranteed,
        Conditional,
        Unknown
    };

    enum class LocationKind : std::uint8_t
    {
        Local,      // alloca slot or SSA value of the analysed function
        ArgPointee, // *arg for a pointer parameter (out-param target)
        Return,     // the value handed to `ret`
        NonLocal    // global, this-field, argument value: storing there escapes
    };

    /// Where an out-parameter target lives relative to a pointer argument: `*(arg + offset)`,
    /// or, via a pointer slot, `**(arg + offset)`. Same encoding as the legacy summaries.
    struct ArgPath
    {
        std::uint64_t offset = 0;
        unsigned argIndex = 0;
        bool viaPointerSlot = false;
        std::uint8_t reservedPadding[3] = {};

        friend bool operator<(const ArgPath& a, const ArgPath& b)
        {
            if (a.argIndex != b.argIndex)
                return a.argIndex < b.argIndex;
            if (a.offset != b.offset)
                return a.offset < b.offset;
            return a.viaPointerSlot < b.viaPointerSlot;
        }
        friend bool operator==(const ArgPath& a, const ArgPath& b)
        {
            return a.argIndex == b.argIndex && a.offset == b.offset &&
                   a.viaPointerSlot == b.viaPointerSlot;
        }
    };

    struct Location
    {
        ArgPath path; // ArgPointee
        LocationKind kind = LocationKind::Local;
        bool strongUpdatable = true; // false when the slot's address is taken
        std::uint8_t reservedPadding[6] = {};
    };

    /// Image of each singleton input state, indexed by OwnState; extends to sets by union.
    /// Uncertainty is sticky and must cross function boundaries with the ownership states.
    struct ParamTransformer
    {
        std::array<StateSet, 4> images{};
        StateSet uncertainInputs;

        constexpr StateSet& operator[](std::size_t i)
        {
            return images[i];
        }
        constexpr const StateSet& operator[](std::size_t i) const
        {
            return images[i];
        }

        [[nodiscard]] constexpr bool isUncertain(StateSet input) const
        {
            return (uncertainInputs.bits & input.bits) != 0;
        }

        friend bool operator==(const ParamTransformer&, const ParamTransformer&) = default;
    };

    constexpr ParamTransformer identityTransformer()
    {
        return {{StateSet::of(OwnState::NotOwned), StateSet::of(OwnState::Owned),
                 StateSet::of(OwnState::Released), StateSet::of(OwnState::Escaped)},
                {}};
    }

    struct FreshResource
    {
        std::string kind; // resource kind of the model rule that created it
        Certainty certainty = Certainty::Unknown;
        std::uint8_t reservedPadding[7] = {};

        friend bool operator==(const FreshResource& a, const FreshResource& b)
        {
            return a.kind == b.kind && a.certainty == b.certainty;
        }
    };

    struct ExitTransformer
    {
        std::map<unsigned, ParamTransformer> params;       // by parameter index (handles by value)
        std::map<ArgPath, ParamTransformer> pointeeParams; // what happens to *(arg+offset)
        std::map<ArgPath, FreshResource> outArgs;          // the path receives a fresh resource
        FreshResource returns;                             // the return value is a fresh resource
        bool present = false;
        std::uint8_t reservedPadding[7] = {};
    };

    struct FunctionOwnershipSummary
    {
        ExitTransformer normal;
        ExitTransformer exceptional;
        bool incomplete = false;
        std::uint8_t reservedPadding[7] = {};
    };

    /// The effects of one modelled or summarised call on the caller's state.
    struct CallEffect
    {
        std::vector<std::pair<LocationId, ParamTransformer>> params;
        std::vector<std::pair<LocationId, Certainty>> outArgs; // each has its own site
        std::vector<std::uint32_t> outArgSites;
        std::optional<LocationId> retDest;
        std::uint32_t site = 0; // acquisition site of the returned resource
        Certainty retCertainty = Certainty::Unknown;
        std::uint8_t reservedPadding[3] = {};
    };

    struct Event
    {
        enum class Kind : std::uint8_t
        {
            Acquire,       // fresh resource `site` into `dst`
            Release,       // release what `src` holds
            Copy,          // dst := src (strong) or dst ∪= src
            Overwrite,     // dst := {Null} / {Unknown}
            Return,        // what `src` holds leaves through `ret`
            AddressEscape, // &dst handed to an unmodelled call
            UnknownCall,   // resources in `args` handed to an unmodelled call
            Call,          // modelled/summarised callee, see `call`
            Exit,          // function exit point (state is recorded, not changed); `args`
                           // lists locations whose resources are uncertain *at this exit*
            // Edge event: the conditional contract of acquisition `site` is decided here;
            // unknownValue = true means "did not acquire" (resource NotOwned, holders null).
            ContractResolved
        };

        std::vector<LocationId> args;
        CallEffect call;
        std::uint32_t site = 0;
        LocationId dst = 0;
        LocationId src = 0;
        std::uint32_t instructionIndex = 0;
        Kind kind = Kind::Exit;
        Certainty certainty = Certainty::Guaranteed;
        bool strong = true;
        bool unknownValue = false;
        bool exceptional = false;
        std::uint8_t reservedPadding[3] = {};
    };

    struct Block
    {
        std::vector<Event> events;
    };

    struct Edge
    {
        std::uint32_t from = 0;
        std::uint32_t to = 0;
        std::vector<Event> events;
    };

    struct OwnershipFacts
    {
        std::vector<Block> blocks; // block 0 is the entry
        std::vector<Edge> edges;
        std::vector<Location> locations;
        /// Summary mode: parameter `first` initially holds resource `second`.
        std::vector<std::pair<unsigned, ResourceId>> paramResources;
        /// Summary mode: the location parameter `first` is passed in.
        std::vector<std::pair<unsigned, LocationId>> paramLocations;
        /// Summary mode: the ArgPointee locations, whose initial contents are summarised too.
        std::vector<std::pair<ArgPath, LocationId>> pointeeLocations;
        std::vector<std::string> siteKinds; // resource kind by site (for summaries)
        std::uint32_t siteCount = 0;
        std::uint32_t reservedPadding = 0;
    };
} // namespace ctrace::stack::analysis::ownership
