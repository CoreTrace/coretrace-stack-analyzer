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
        using const_suggestion::itemOf;
        using const_suggestion::Judgement;
        using const_suggestion::Named;
        using const_suggestion::UnitFacts;
        using const_suggestion::Verdict;

        // Appends `item` once, in the order items come.
        void addOnce(std::vector<std::string>& items, const std::string& item)
        {
            if (std::find(items.begin(), items.end(), item) == items.end())
                items.push_back(item);
        }
    } // namespace

    std::string const_suggestion::itemOf(const std::vector<std::string>& value,
                                         std::string_view prefix)
    {
        for (const std::string& item : value)
        {
            if (item.starts_with(prefix))
                return item.substr(prefix.size());
        }
        return "";
    }

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
        // The references at each key, in the order of the traversal (Task 3.2, O1): those after
        // the rewrite, and those before it at a key without instantiation context, which no pair
        // changes, are grouped once; the others go through each round's pairs.
        struct At
        {
            std::size_t first = 0; // the position of its first reference before the rewrite
            FactKey key;
            std::vector<const Fact*> facts;
        };
        std::map<FactKey, std::vector<const Fact*>> nowAt;
        for (const Fact& fact : after.facts)
        {
            if (fact.kind == FactKind::Reference)
                nowAt[fact.key].push_back(&fact);
        }
        std::vector<At> fixed;
        std::vector<std::pair<std::size_t, const Fact*>> contextual;
        {
            std::map<FactKey, std::size_t> slot;
            for (std::size_t i = 0; i < before.facts.size(); ++i)
            {
                const Fact& fact = before.facts[i];
                if (fact.kind != FactKind::Reference)
                    continue;
                if (!fact.key.contexts.empty())
                {
                    contextual.emplace_back(i, &fact);
                    continue;
                }
                const auto [it, added] = slot.try_emplace(fact.key, fixed.size());
                if (added)
                    fixed.push_back({i, fact.key, {}});
                fixed[it->second].facts.push_back(&fact);
            }
        }
        // Steps 3, 4 and 5 run to one common fixpoint: a pair that one establishes may map what
        // another needs.
        for (std::size_t settled = static_cast<std::size_t>(-1); settled != pairs.size();)
        {
            settled = pairs.size();
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
                std::vector<At> moving;
                {
                    std::map<FactKey, std::size_t> slot;
                    for (const auto& [i, fact] : contextual)
                    {
                        if (const std::optional<FactKey> key = forward(fact->key))
                        {
                            const auto [it, added] = slot.try_emplace(*key, moving.size());
                            if (added)
                                moving.push_back({i, *key, {}});
                            moving[it->second].facts.push_back(fact);
                        }
                    }
                }
                // Both, merged in the order of their first references: a key with contexts never
                // equals one without.
                std::vector<const At*> order;
                order.reserve(fixed.size() + moving.size());
                for (std::size_t a = 0, b = 0; a < fixed.size() || b < moving.size();)
                {
                    if (b == moving.size() ||
                        (a < fixed.size() && fixed[a].first < moving[b].first))
                        order.push_back(&fixed[a++]);
                    else
                        order.push_back(&moving[b++]);
                }
                std::vector<std::string> proposing;                   // in the order of the facts
                std::map<std::string, std::vector<std::string>> to;   // before to after
                std::map<std::string, std::vector<std::string>> from; // after to before
                std::map<std::string, std::string> shown;
                for (const At* at : order)
                {
                    const std::vector<const Fact*>& wasFacts = at->facts;
                    const auto nowFacts = nowAt.find(at->key);
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
                            {
                                return shown.count(symbol) ? shown[symbol] : llvm::demangle(symbol);
                            };
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
                    const auto other = std::find_if(
                        now->nested.begin(), now->nested.end(), [&](const Named& named)
                        { return named.key == one.key && named.enclosing == enclosing; });
                    if (other != now->nested.end())
                        added = pair(one.symbol, other->symbol) || added;
                }
            }

            // 5: the types nested in every established pair (lambdas, local classes) and what they
            // reach: the members of those types, the specializations whose arguments or type hold
            // them, and the implicit members of the classes that hold them, by the identities the
            // facts give (UnitFacts::Identity). A key's nested types are mapped through the pairs;
            // every other part of it is kept as it is. One to one: a key with more than one symbol
            // on either side gives no pair, and a reason.
            {
                const auto substitute = [&](const std::string& key)
                {
                    std::string out;
                    for (std::size_t at = 0; at < key.size();)
                    {
                        const std::size_t open = key.find('\x02', at);
                        if (open == std::string::npos)
                        {
                            out += key.substr(at);
                            break;
                        }
                        const std::size_t close = key.find('\x03', open);
                        out += key.substr(at, open + 1 - at);
                        const std::string enclosing = key.substr(open + 1, close - open - 1);
                        const auto mapped = pairs.find(enclosing);
                        out += mapped == pairs.end() ? enclosing : mapped->second;
                        at = close;
                    }
                    return out;
                };
                // Only the symbols that the rewrite renames: one side has them, the other not.
                std::set<std::string> beforeNames, afterNames;
                for (const UnitFacts::Identity& identity : before.identities)
                    beforeNames.insert(identity.symbol);
                for (const UnitFacts::Identity& identity : after.identities)
                    afterNames.insert(identity.symbol);
                std::map<std::string, std::vector<std::string>> nowAtKey; // key -> symbols after
                for (const UnitFacts::Identity& identity : after.identities)
                {
                    if (taken.count(identity.symbol) || beforeNames.count(identity.symbol))
                        continue;
                    if (!identity.nested.empty())
                        nowAtKey["n" + identity.nested + '\0' + identity.enclosing].push_back(
                            identity.symbol);
                    if (!identity.specialization.empty())
                        nowAtKey["s" + identity.specialization].push_back(identity.symbol);
                }
                std::map<std::string, std::vector<std::string>>
                    wasAtKey; // mapped key -> symbols before
                for (const UnitFacts::Identity& identity : before.identities)
                {
                    if (pairs.count(identity.symbol) || afterNames.count(identity.symbol))
                        continue;
                    if (!identity.nested.empty())
                    {
                        const auto around = pairs.find(identity.enclosing);
                        if (around != pairs.end())
                            wasAtKey["n" + substitute(identity.nested) + '\0' + around->second]
                                .push_back(identity.symbol);
                    }
                    else if (!identity.specialization.empty())
                        wasAtKey["s" + substitute(identity.specialization)].push_back(
                            identity.symbol);
                }
                for (const auto& [key, sources] : wasAtKey)
                {
                    const auto found = nowAtKey.find(key);
                    if (found == nowAtKey.end())
                        continue;
                    if (sources.size() == 1 && found->second.size() == 1)
                        pair(sources.front(), found->second.front());
                    else if (missing.empty())
                        missing = "'" + llvm::demangle(sources.front()) +
                                  "' has no one-to-one counterpart through its template arguments";
                }
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

    Judgement const_suggestion::judgeCollisions(const std::vector<const UnitFacts*>& units,
                                                std::size_t entity, const std::string& definingFile,
                                                const std::string& function)
    {
        using llvm::GlobalValue;
        const auto indeterminate = [](std::string why)
        { return Judgement{std::move(why), Verdict::Indeterminate}; };
        // Strong: external; weak: weak or linkonce, ODR or not. Any other linkage, local above
        // all, has no scope across files.
        const auto strong = [](GlobalValue::LinkageTypes linkage)
        { return GlobalValue::isExternalLinkage(linkage); };
        const auto weak = [](GlobalValue::LinkageTypes linkage)
        { return GlobalValue::isWeakLinkage(linkage) || GlobalValue::isLinkOnceLinkage(linkage); };
        // The entity's own linkage, as the defining unit defines it; strong if it shows none.
        GlobalValue::LinkageTypes own = GlobalValue::ExternalLinkage;
        for (const UnitFacts* unit : units)
        {
            if (unit->file == definingFile && entity < unit->entities.size() &&
                unit->entities[entity].linkage)
            {
                own = *unit->entities[entity].linkage;
            }
        }
        if (!strong(own) && !weak(own))
            return {};
        // A definition first, then a declaration (ruling 1), in the order the units are judged.
        for (const UnitFacts* unit : units)
        {
            if (entity >= unit->entities.size())
                continue;
            for (const UnitFacts::Entity::Collision& collision : unit->entities[entity].collisions)
            {
                if (!collision.linkage ||
                    (!strong(*collision.linkage) && !weak(*collision.linkage)))
                    continue;
                const std::string assumption = collision.symbol + " is also defined in " +
                                               unit->file + "; if " + definingFile + " and " +
                                               unit->file + " are linked together, ";
                if (strong(own) && strong(*collision.linkage))
                    return indeterminate(assumption + "the two definitions collide");
                if (weak(own) && weak(*collision.linkage))
                {
                    return indeterminate(
                        assumption + "the linker keeps one of the two weak definitions, and the "
                                     "check establishes neither which one nor that they are "
                                     "equivalent");
                }
                const bool ownIsStrong = strong(own);
                return indeterminate(assumption + "the strong definition, in " +
                                     (ownIsStrong ? definingFile : unit->file) +
                                     ", is used and the weak one, in " +
                                     (ownIsStrong ? unit->file : definingFile) + ", is ignored");
            }
        }
        for (const UnitFacts* unit : units)
        {
            if (entity >= unit->entities.size())
                continue;
            for (const UnitFacts::Entity::Collision& collision : unit->entities[entity].collisions)
            {
                if (collision.linkage)
                    continue;
                return indeterminate("the rewrite gives " + function +
                                     " the symbol of another declaration (" +
                                     collision.site.expansion.file + ":" +
                                     std::to_string(collision.line) + "); if a file linked with " +
                                     definingFile + " defines it, the two definitions collide");
            }
        }
        return {};
    }
} // namespace ctrace::stack::analysis
