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

    struct Location
    {
        unsigned argIndex = 0; // ArgPointee
        LocationKind kind = LocationKind::Local;
        bool strongUpdatable = true; // false when the slot's address is taken
        std::uint8_t reservedPadding[2] = {};
    };

    /// Image of each singleton input state, indexed by OwnState; extends to sets by union.
    using ParamTransformer = std::array<StateSet, 4>;

    constexpr ParamTransformer identityTransformer()
    {
        return {StateSet::of(OwnState::NotOwned), StateSet::of(OwnState::Owned),
                StateSet::of(OwnState::Released), StateSet::of(OwnState::Escaped)};
    }

    struct ExitTransformer
    {
        std::map<unsigned, ParamTransformer> params; // by parameter index
        Certainty returns = Certainty::Unknown;      // the return value is a fresh resource
        std::map<unsigned, Certainty> outArgs;       // *arg receives a fresh resource
        bool present = false;
    };

    struct FunctionOwnershipSummary
    {
        ExitTransformer normal;
        ExitTransformer exceptional;
        bool incomplete = false;
    };

    /// The effects of one modelled or summarised call on the caller's state.
    struct CallEffect
    {
        std::vector<std::pair<LocationId, ParamTransformer>> params;
        std::vector<std::pair<LocationId, Certainty>> outArgs;
        std::optional<LocationId> retDest;
        std::uint32_t site = 0; // acquisition site of the fresh resources
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
            Exit           // function exit point (state is recorded, not changed)
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
        std::uint32_t siteCount = 0;
        std::uint32_t reservedPadding = 0;
    };
} // namespace ctrace::stack::analysis::ownership
