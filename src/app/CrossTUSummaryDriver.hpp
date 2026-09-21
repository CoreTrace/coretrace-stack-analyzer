// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ctrace::stack::app::detail
{
    // ── Tarjan SCC algorithm on module indices ──
    // Used by both resource and uninit cross-TU loops to compute strongly
    // connected components of the inter-module call graph.
    struct ModuleTarjan
    {
        std::vector<int> index;
        std::vector<int> lowlink;
        std::vector<bool> onStack;
        std::vector<std::size_t> stack;
        std::vector<std::vector<std::size_t>> sccs;
        int nextIndex = 0;
        std::byte padding1[64 - sizeof(int)]{}; // cache line isolation

        void run(std::size_t N, const std::vector<std::unordered_set<std::size_t>>& edges)
        {
            index.assign(N, -1);
            lowlink.assign(N, -1);
            onStack.assign(N, false);
            stack.reserve(N);
            for (std::size_t v = 0; v < N; ++v)
            {
                if (index[v] < 0)
                    strongConnect(v, edges);
            }
        }

        void strongConnect(std::size_t v, const std::vector<std::unordered_set<std::size_t>>& edges)
        {
            index[v] = lowlink[v] = nextIndex++;
            stack.push_back(v);
            onStack[v] = true;

            for (std::size_t w : edges[v])
            {
                if (index[w] < 0)
                {
                    strongConnect(w, edges);
                    lowlink[v] = std::min(lowlink[v], lowlink[w]);
                }
                else if (onStack[w])
                {
                    lowlink[v] = std::min(lowlink[v], index[w]);
                }
            }

            if (lowlink[v] == index[v])
            {
                std::vector<std::size_t> component;
                std::size_t w;
                do
                {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = false;
                    component.push_back(w);
                } while (w != v);
                // Sort for deterministic output.
                std::sort(component.begin(), component.end());
                sccs.push_back(std::move(component));
            }
        }
    };

    // Build the single-def filtered inter-module edge graph.
    // Only edges through functions with exactly one definition across all modules
    // are included. Multi-def functions (inline, template, weak) are excluded
    // because their summaries are identical in all TUs and don't create real
    // cross-module data dependencies.
    static std::vector<std::unordered_set<std::size_t>> buildSingleDefFilteredEdges(
        std::size_t N, const std::vector<std::unordered_set<std::string>>& moduleCalleeNames,
        const std::unordered_map<std::string, std::vector<std::size_t>>& definedBy)
    {
        std::vector<std::unordered_set<std::size_t>> edges(N);
        for (std::size_t i = 0; i < N; ++i)
        {
            for (const std::string& callee : moduleCalleeNames[i])
            {
                auto it = definedBy.find(callee);
                if (it == definedBy.end() || it->second.size() != 1)
                    continue; // skip multi-def and unresolved
                const std::size_t j = it->second[0];
                if (j != i)
                    edges[i].insert(j);
            }
        }
        return edges;
    }

    // Run Tarjan on the filtered edges and return SCCs in dependency order
    // (callees before callers).
    // With edge direction caller→callee, Tarjan's DFS reaches callees first
    // and emits their SCCs before caller SCCs.  This is already the correct
    // processing order — no reversal needed.
    static std::vector<std::vector<std::size_t>>
    computeTopologicalSCCOrder(std::size_t N,
                               const std::vector<std::unordered_set<std::size_t>>& filteredEdges)
    {
        ModuleTarjan tarjan;
        tarjan.run(N, filteredEdges);
        return std::move(tarjan.sccs);
    }

    // Compute topological levels for SCCs given the filtered edge graph.
    // SCCs at the same level are independent and can be processed in parallel.
    // Returns a vector of levels (one per SCC in sccOrder), plus fills
    // levelGroups: levelGroups[level] = {indices into sccOrder}.
    static std::vector<unsigned>
    computeSCCLevels(const std::vector<std::vector<std::size_t>>& sccOrder,
                     const std::vector<std::unordered_set<std::size_t>>& filteredEdges,
                     std::size_t N)
    {
        // Map each module index to its SCC index in sccOrder.
        std::vector<std::size_t> moduleToSCC(N);
        for (std::size_t s = 0; s < sccOrder.size(); ++s)
            for (std::size_t m : sccOrder[s])
                moduleToSCC[m] = s;

        std::vector<unsigned> level(sccOrder.size(), 0);
        // Process in topological order: each SCC's level is 1 + max(predecessor levels).
        for (std::size_t s = 0; s < sccOrder.size(); ++s)
        {
            unsigned maxPred = 0;
            bool hasPred = false;
            for (std::size_t m : sccOrder[s])
            {
                for (std::size_t dep : filteredEdges[m])
                {
                    const std::size_t depSCC = moduleToSCC[dep];
                    if (depSCC != s)
                    {
                        hasPred = true;
                        maxPred = std::max(maxPred, level[depSCC]);
                    }
                }
            }
            level[s] = hasPred ? maxPred + 1 : 0;
        }
        return level;
    }

    /// Shared scheduling plan; all summaries use the same single-definition dependency rule.
    struct CrossTUSummaryPlan
    {
        struct Level
        {
            std::vector<std::size_t> trivialModules;
            std::vector<std::size_t> cyclicSCCs;
            std::size_t moduleCount = 0;
        };

        std::vector<std::vector<std::size_t>> sccs;
        std::vector<Level> levels;
        std::vector<std::unordered_set<std::string>> filteredCalleeNames;
        std::size_t trivialCount = 0;
        std::size_t cyclicCount = 0;

        CrossTUSummaryPlan(
            const std::vector<std::unordered_set<std::string>>& callees,
            const std::unordered_map<std::string, std::vector<std::size_t>>& definitions)
            : filteredCalleeNames(callees.size())
        {
            const auto edges = buildSingleDefFilteredEdges(callees.size(), callees, definitions);
            sccs = computeTopologicalSCCOrder(callees.size(), edges);
            const auto sccLevels = computeSCCLevels(sccs, edges, callees.size());
            unsigned maxLevel = 0;
            for (unsigned level : sccLevels)
                maxLevel = std::max(maxLevel, level);
            levels.resize(maxLevel + 1);
            for (std::size_t i = 0; i < callees.size(); ++i)
            {
                for (const auto& callee : callees[i])
                {
                    const auto it = definitions.find(callee);
                    if (it != definitions.end() && it->second.size() == 1)
                        filteredCalleeNames[i].insert(callee);
                }
            }
            for (std::size_t s = 0; s < sccs.size(); ++s)
            {
                auto& level = levels[sccLevels[s]];
                level.moduleCount += sccs[s].size();
                if (sccs[s].size() == 1 && !edges[sccs[s][0]].count(sccs[s][0]))
                {
                    level.trivialModules.push_back(sccs[s][0]);
                    ++trivialCount;
                }
                else
                {
                    level.cyclicSCCs.push_back(s);
                    ++cyclicCount;
                }
            }
            for (auto& level : levels)
                std::sort(level.cyclicSCCs.begin(), level.cyclicSCCs.end(),
                          [&](std::size_t a, std::size_t b) { return sccs[a][0] < sccs[b][0]; });
        }
    };

    /// One dependency-ordered pass. Operations own summary semantics/preparation and caching;
    /// the driver owns ordering, dirty propagation and the SCC iteration budget.
    /// Parallel builds only read the external index. All merges and cache writes are serial.
    template <typename Operations, typename ParallelFor>
    std::size_t runCrossTUSummaryPass(const CrossTUSummaryPlan& plan,
                                      typename Operations::Index& globalIndex,
                                      std::vector<typename Operations::Index>& moduleSummaries,
                                      Operations& operations, ParallelFor&& parallelFor)
    {
        using Index = typename Operations::Index;
        using Clock = std::chrono::steady_clock;
        constexpr unsigned kMaxSCCIterations = 12;
        std::size_t analyses = 0;
        for (std::size_t levelIndex = 0; levelIndex < plan.levels.size(); ++levelIndex)
        {
            const auto& level = plan.levels[levelIndex];
            if (level.moduleCount == 0)
                continue;
            const auto start = Clock::now();
            const auto external = operations.prepareLevel(globalIndex);
            std::vector<std::size_t> missing;
            for (std::size_t module : level.trivialModules)
                if (!operations.tryCache(module, external, moduleSummaries[module]))
                    missing.push_back(module);
            parallelFor(
                missing, [&](std::size_t module)
                { moduleSummaries[module] = operations.build(module, globalIndex, external); });
            for (std::size_t module : missing)
                operations.cache(module, external, moduleSummaries[module]);
            analyses += missing.size();
            for (std::size_t module : level.trivialModules)
                operations.merge(globalIndex, moduleSummaries[module]);

            for (std::size_t sccIndex : level.cyclicSCCs)
            {
                const auto& scc = plan.sccs[sccIndex];
                std::vector<Index> previous(moduleSummaries.size());
                std::unordered_set<std::string> changedNames;
                bool converged = false;
                for (unsigned iteration = 0; iteration < kMaxSCCIterations; ++iteration)
                {
                    const auto cyclicExternal = operations.prepareCycle(globalIndex);
                    std::vector<std::size_t> dirty;
                    for (std::size_t module : scc)
                    {
                        const auto& callees = plan.filteredCalleeNames[module];
                        if (iteration == 0 ||
                            std::any_of(callees.begin(), callees.end(), [&](const auto& callee)
                                        { return changedNames.count(callee) != 0; }))
                            dirty.push_back(module);
                        else
                            moduleSummaries[module] = previous[module];
                    }
                    for (std::size_t module : dirty)
                        moduleSummaries[module] =
                            operations.build(module, globalIndex, cyclicExternal);
                    analyses += dirty.size();
                    Index merged;
                    Index previousMerged;
                    for (std::size_t module : scc)
                    {
                        operations.merge(merged, moduleSummaries[module]);
                        operations.merge(previousMerged, previous[module]);
                    }
                    converged = operations.equals(merged, previousMerged);
                    changedNames = operations.changedNames(previousMerged, merged);
                    for (std::size_t module : scc)
                        previous[module] = moduleSummaries[module];
                    operations.reportIteration(scc.size(), iteration + 1, converged, dirty.size());
                    if (converged)
                        break;
                    for (std::size_t module : scc)
                        operations.merge(globalIndex, moduleSummaries[module]);
                }
                if (!converged)
                    operations.reportLimit(scc.size(), kMaxSCCIterations);
                for (std::size_t module : scc)
                    operations.merge(globalIndex, moduleSummaries[module]);
            }
            operations.reportLevel(
                levelIndex, level,
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start)
                    .count());
        }
        return analyses;
    }
} // namespace ctrace::stack::app::detail
