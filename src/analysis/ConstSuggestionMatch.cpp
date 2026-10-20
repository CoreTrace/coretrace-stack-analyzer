// SPDX-License-Identifier: Apache-2.0
// The correspondences of a rewrite (#217, §5.7): which symbols of a unit's compilation after the
// rewrite are those of its compilation before it, established from compilation facts only.
#include "ConstSuggestionInternal.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/Demangle/Demangle.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::Fact;
        using const_suggestion::FactKey;
        using const_suggestion::FactKind;
        using const_suggestion::Named;
        using const_suggestion::UnitFacts;

        // The item of an R value that starts with `prefix`, without it; "" when it has none.
        std::string itemOf(const std::vector<std::string>& value, std::string_view prefix)
        {
            for (const std::string& item : value)
            {
                if (item.starts_with(prefix))
                    return item.substr(prefix.size());
            }
            return "";
        }

        // Appends `item` once, in the order items come.
        void addOnce(std::vector<std::string>& items, const std::string& item)
        {
            if (std::find(items.begin(), items.end(), item) == items.end())
                items.push_back(item);
        }
    } // namespace

    std::vector<SymbolPair> const_suggestion::correspond(const UnitFacts& before,
                                                         std::size_t entity, const UnitFacts& after,
                                                         std::string& missing)
    {
        std::map<std::string, std::string> pairs; // before the rewrite, to after it
        std::set<std::string> taken;              // the symbols after it, once paired
        const auto pair = [&](const std::string& was, const std::string& now)
        {
            if (was.empty() || now.empty() || pairs.count(was) || taken.count(now))
                return false;
            pairs[was] = now;
            taken.insert(now);
            return true;
        };
        const UnitFacts::Entity* was =
            entity < before.entities.size() ? &before.entities[entity] : nullptr;
        const UnitFacts::Entity* now = after.entities.empty() ? nullptr : &after.entities[0];

        // 1 and 2: the entity's variants, by variant; the constructors inheriting it, by derived
        // class, inherited specialization and variant.
        const auto byKey =
            [&](const std::vector<Named>& wasNamed, const std::vector<Named>& nowNamed)
        {
            for (const Named& one : wasNamed)
            {
                const auto other =
                    std::find_if(nowNamed.begin(), nowNamed.end(),
                                 [&](const Named& named) { return named.key == one.key; });
                if (other != nowNamed.end())
                    pair(one.symbol, other->symbol);
            }
        };
        if (was && now)
        {
            byKey(was->variants, now->variants);
            byKey(was->inheriting, now->inheriting);
        }

        // 3: the instantiation contexts. A reference whose instantiations already map, and whose
        // target becomes another specialization of the same template, proposes that pair; the
        // proposals of every reference are checked together, and each new pair maps more
        // instantiations, until none is added.
        for (bool added = true; added;)
        {
            added = false;
            const auto forward = [&](FactKey key) -> std::optional<FactKey>
            {
                for (std::string& context : key.contexts)
                {
                    const auto found = pairs.find(context);
                    if (found != pairs.end())
                        context = found->second;
                    else if (!after.instantiations.count(context))
                        return std::nullopt;
                }
                return key;
            };
            std::vector<FactKey> order;
            std::map<FactKey, std::vector<const Fact*>> wasAt;
            std::map<FactKey, std::vector<const Fact*>> nowAt;
            for (const Fact& fact : before.facts)
            {
                if (fact.kind != FactKind::Reference)
                    continue;
                if (const std::optional<FactKey> key = forward(fact.key))
                {
                    if (!wasAt.count(*key))
                        order.push_back(*key);
                    wasAt[*key].push_back(&fact);
                }
            }
            for (const Fact& fact : after.facts)
            {
                if (fact.kind == FactKind::Reference)
                    nowAt[fact.key].push_back(&fact);
            }
            std::vector<std::string> proposing;                   // in the order of the facts
            std::map<std::string, std::vector<std::string>> to;   // before to after
            std::map<std::string, std::vector<std::string>> from; // after to before
            std::map<std::string, std::string> shown;
            for (const FactKey& key : order)
            {
                const std::vector<const Fact*>& wasFacts = wasAt[key];
                const auto nowFacts = nowAt.find(key);
                if (nowFacts == nowAt.end() || nowFacts->second.size() != wasFacts.size())
                    continue;
                for (std::size_t k = 0; k < wasFacts.size(); ++k)
                {
                    const Fact& one = *wasFacts[k];
                    const Fact& other = *nowFacts->second[k];
                    const std::string wasTarget = itemOf(one.value, "symbol:");
                    const std::string nowTarget = itemOf(other.value, "symbol:");
                    const std::string pattern = itemOf(one.value, "template:");
                    if (wasTarget.empty() || nowTarget.empty() || pattern.empty() ||
                        wasTarget == nowTarget || pattern != itemOf(other.value, "template:") ||
                        pairs.count(wasTarget))
                    {
                        continue;
                    }
                    addOnce(proposing, wasTarget);
                    addOnce(to[wasTarget], nowTarget);
                    addOnce(from[nowTarget], wasTarget);
                    shown[wasTarget] = one.shown;
                    shown[nowTarget] = other.shown;
                }
            }
            for (const std::string& one : proposing)
            {
                const std::vector<std::string>& targets = to[one];
                if (targets.size() > 1)
                {
                    if (missing.empty())
                    {
                        missing = "'" + shown[one] + "' becomes both '" + shown[targets[0]] +
                                  "' and '" + shown[targets[1]] +
                                  "': their correspondence is not one to one";
                    }
                    continue;
                }
                // Two before the rewrite that become one, or one that becomes a symbol already
                // paired.
                std::vector<std::string> sources = from[targets[0]];
                for (const auto& [wasSymbol, nowSymbol] : pairs)
                {
                    if (nowSymbol == targets[0])
                        sources.insert(sources.begin(), wasSymbol);
                }
                if (sources.size() > 1)
                {
                    if (missing.empty())
                    {
                        const auto name = [&](const std::string& symbol)
                        { return shown.count(symbol) ? shown[symbol] : llvm::demangle(symbol); };
                        missing = "'" + name(sources[0]) + "' and '" + name(sources[1]) +
                                  "' both become '" + name(targets[0]) +
                                  "': their correspondence is not one to one";
                    }
                    continue;
                }
                added = pair(one, targets[0]) || added;
            }
        }

        // 4: the nested symbols, by kind and site, once the symbols around them are paired.
        for (bool added = was && now; added;)
        {
            added = false;
            for (const Named& one : was->nested)
            {
                if (pairs.count(one.symbol))
                    continue;
                const auto around = pairs.find(one.enclosing);
                const std::string enclosing =
                    around == pairs.end() ? one.enclosing : around->second;
                const auto other =
                    std::find_if(now->nested.begin(), now->nested.end(), [&](const Named& named)
                                 { return named.key == one.key && named.enclosing == enclosing; });
                if (other != now->nested.end())
                    added = pair(one.symbol, other->symbol) || added;
            }
        }

        std::vector<SymbolPair> out;
        for (const auto& [wasSymbol, nowSymbol] : pairs)
        {
            if (wasSymbol != nowSymbol)
                out.push_back({wasSymbol, nowSymbol});
        }
        return out;
    }
} // namespace ctrace::stack::analysis
