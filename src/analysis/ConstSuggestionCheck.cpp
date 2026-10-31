// SPDX-License-Identifier: Apache-2.0
// Checks the const suggestions of ConstParameterNotModified with Clang (#217): the analysis of the
// IR decides whether an execution writes the pointee; Clang decides whether the suggested type
// compiles in every analyzed file and leaves the generated code as it is.
#include "analysis/ConstSuggestionCheck.hpp"

#include "ConstSuggestionInternal.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/CompileCommands.hpp"
#include "analysis/InputPipeline.hpp"
#include "analyzer/HotspotProfiler.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <compilerlib/toolchain.hpp>
#include <coretrace/logger.hpp>
#include <llvm/ADT/StringExtras.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/xxhash.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using const_suggestion::DeclarationSite;
        using const_suggestion::Diagnostic;
        using const_suggestion::Edit;
        using const_suggestion::Fact;
        using const_suggestion::FactKey;
        using const_suggestion::FactKind;
        using const_suggestion::inspect;
        using const_suggestion::Judgement;
        using const_suggestion::realPath;
        using const_suggestion::scan;
        using const_suggestion::SourceLinkage;
        using const_suggestion::SuggestionPlan;
        using const_suggestion::Target;
        using const_suggestion::UnitFacts;
        using const_suggestion::Verdict;

        // What the check covers: the analyzed files, or every file of the compilation database.
        constexpr const char* kCheckedAnalyzed =
            "checked in the analyzed files only, with their compile commands: ";
        constexpr const char* kCheckedDatabase =
            "checked in every file of the compilation database that sees the change, with its "
            "compile command: ";
        // What it verifies (README, "Const parameter suggestions checked with Clang").
        constexpr const char* kCheckedResult =
            "they compile with the suggested type, and their generated code matches, metadata "
            "included, once debug information, diagnostic metadata, descriptive names and "
            "renamed symbols are set aside";
        // For an rvalue reference, the const reference only (D12).
        constexpr const char* kCheckedReference =
            "they compile with the suggested const reference, and their generated code matches, "
            "metadata included, once debug information, diagnostic metadata, descriptive names "
            "and renamed symbols are set aside; passing by value is not checked";
        constexpr const char* kNotChecked = "not checked with Clang: ";
        constexpr const char* kDoesNotCompileAsItIs = "a file does not compile with Clang as it is";

        // A unit before the rewrite, prepared once for every suggestion of the module: what its
        // compilation shows, its module released, and its generated code ready to compare.
        struct Original
        {
            UnitFacts unit;
            std::shared_ptr<const const_suggestion::ModuleText> code;
        };

        Original originalFrom(UnitFacts unit, bool timing)
        {
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.prepare");
            auto code = std::make_shared<const const_suggestion::ModuleText>(
                const_suggestion::prepare(*unit.module));
            unit.module.reset();
            unit.context.reset();
            return {std::move(unit), std::move(code)};
        }

        bool commandFor(const AnalyzedUnit& unit, const AnalysisConfig& config, UnitCommand& out,
                        std::string& error)
        {
            std::vector<std::string> args;
            if (!buildSourceCompileCommand(unit.filename, unit.language, config, args,
                                           out.workingDir, error))
            {
                return false;
            }
            // Options only compilerlib understands.
            std::erase_if(args, [](const std::string& arg) { return arg.starts_with("--ct-"); });
            compilerlib::DriverConfig driver;
            if (!compilerlib::resolveDriverConfig(args, driver, error))
                return false;
            out.args = {driver.clang_path};
            if (driver.force_cxx_driver)
                out.args.push_back("--driver-mode=g++");
            if (driver.add_resource_dir)
            {
                out.args.push_back("-resource-dir");
                out.args.push_back(driver.resource_dir);
            }
            if (driver.add_sysroot)
            {
                out.args.push_back("-isysroot");
                out.args.push_back(driver.sysroot);
            }
            out.args.insert(out.args.end(), args.begin(), args.end());
            return true;
        }

        // An input as a real absolute path: relative inputs resolve from the input directory.
        std::string canonicalPath(const std::string& filename, const AnalysisConfig& config)
        {
            llvm::SmallString<256> path(filename);
            if (!llvm::sys::path::is_absolute(path))
            {
                llvm::SmallString<256> base(config.inputBaseDir);
                if (base.empty())
                    llvm::sys::fs::current_path(base);
                llvm::sys::path::append(base, filename);
                path = base;
            }
            return realPath(path);
        }

        bool references(const llvm::Module& module, llvm::StringRef symbol)
        {
            const llvm::GlobalValue* value = module.getNamedValue(symbol);
            return value && !value->hasLocalLinkage();
        }

    } // namespace

    std::map<std::string, std::string> const_suggestion::rewrite(const SuggestionPlan& plan,
                                                                 std::string& failure)
    {
        std::map<std::string, std::string> out;
        for (const auto& [path, edits] : plan.edits)
        {
            llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
                llvm::MemoryBuffer::getFile(path);
            if (!buffer)
            {
                failure = path + " cannot be read";
                return out;
            }
            std::string text = (*buffer)->getBuffer().str();
            std::vector<Edit> sorted = edits;
            std::sort(sorted.begin(), sorted.end(),
                      [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
            for (const Edit& edit : sorted)
            {
                if (edit.offset > text.size() || edit.length > text.size() - edit.offset)
                {
                    failure = path + " changed after the plan was made";
                    return out;
                }
                text.replace(edit.offset, edit.length, edit.text);
            }
            out[path] = std::move(text);
        }
        return out;
    }

    namespace
    {
        // A unit before the rewrite, by its file and its configuration: compiled once for every
        // suggestion of the module (#217, §5, step 3).
        using OriginalCache = std::map<std::string, std::optional<Original>>;

        const std::optional<Original>& originalOf(const UnitCommand& command,
                                                  const std::string& key,
                                                  const std::vector<Target>& planned,
                                                  OriginalCache& cache, bool timing)
        {
            std::optional<Original>& out =
                cache[key + '\0' + const_suggestion::fingerprintOf(command)];
            if (out)
                return out;
            UnitFacts before = inspect(command, nullptr, {}, planned,
                                       "const_suggestion.compile.other_before", timing);
            if (!before.failure.empty() || !before.module)
                return out;
            out = originalFrom(std::move(before), timing);
            return out;
        }

        // The reason a fact gives once the rewrite changes it (Texts).
        std::string changeOf(const Fact& before, const Fact& after)
        {
            const std::string place =
                before.key.site.expansion.file + ":" + std::to_string(before.line);
            if (before.kind == FactKind::Reference)
                return place + " refers to '" + after.shown + "' instead of '" + before.shown + "'";
            if (before.kind == FactKind::Override)
            {
                return "'" + before.subject + "' overrides " + after.shown + " instead of " +
                       before.shown;
            }
            if (before.subject == "_Generic")
                return "the _Generic selection at " + place + " chooses another association";
            return "the condition at " + place + " becomes " + after.value.front();
        }

        // What step 6 finds: the first fact that the rewrite changes, and else, when a site's
        // facts cannot be compared one to one, why the comparison is incomplete.
        struct FactComparison
        {
            std::optional<std::string> changed;
            std::optional<std::string> incomplete;
        };

        // Step 6 (§5.8): the facts R and K, then O, compared key by key, one to one in the order
        // of the traversal: one key can hold several facts, such as a macro argument expanded
        // twice. A key with as many facts on both sides is compared completely; one with a
        // different number, a fact on one side only included, leaves the comparison incomplete.
        // Keys go through the entity's own symbol pairs: a fact in an instantiation they do not
        // map is left out, and so is a reference whose targets are specializations or implicit
        // functions on both sides; Task 1.5 pairs them.
        FactComparison compareFacts(const UnitFacts& before, std::size_t index,
                                    const UnitFacts& after, const std::vector<SymbolPair>& pairs)
        {
            std::map<std::string, std::string> renamed; // the established pairs, before to after
            std::map<std::string, std::string> back;
            for (const SymbolPair& pair : pairs)
            {
                renamed[pair.before] = pair.after;
                back[pair.after] = pair.before;
            }
            const UnitFacts::Entity* entity = after.entities.empty() ? nullptr : &after.entities[0];
            // A key before the rewrite, as the compilation after it writes it.
            const auto forward = [&](FactKey key) -> std::optional<FactKey>
            {
                for (std::string& context : key.contexts)
                {
                    const auto pair = renamed.find(context);
                    if (pair != renamed.end())
                        context = pair->second;
                    else if (!after.instantiations.count(context))
                        return std::nullopt;
                }
                return key;
            };
            // Whether a key after the rewrite can have a counterpart before it.
            const auto known = [&](const FactKey& key)
            {
                return std::all_of(
                    key.contexts.begin(), key.contexts.end(), [&](const std::string& context)
                    { return back.count(context) || before.instantiations.count(context); });
            };
            const auto mappedBack = [&](std::vector<std::string> value)
            {
                for (std::string& item : value)
                {
                    const auto pair =
                        item.starts_with("symbol:") ? back.find(item.substr(7)) : back.end();
                    if (pair != back.end())
                        item = "symbol:" + pair->second;
                }
                std::sort(value.begin(), value.end());
                return value;
            };
            const auto sites = [](const std::vector<std::string>& value)
            {
                return std::all_of(value.begin(), value.end(), [](const std::string& item)
                                   { return item.starts_with("site:"); });
            };
            const auto changed = [&](const Fact& was, const Fact& now)
            {
                if (was.kind != FactKind::Reference)
                    return mappedBack(now.value) != was.value;
                // Unchanged when its targets after the rewrite keep every declaration they had
                // (ruling 4).
                if (sites(was.value) && sites(now.value))
                {
                    return !std::includes(now.value.begin(), now.value.end(), was.value.begin(),
                                          was.value.end());
                }
                if (sites(was.value) != sites(now.value))
                    return true;
                // Two symbols: the same target through the pairs; else a change only between two
                // templates, or between a specialization and a function of no template. Two
                // specializations of one template are the correspondences' to pair (§5.7).
                if (mappedBack(now.value) == was.value)
                    return false;
                const std::string wasTemplate = const_suggestion::itemOf(was.value, "template:");
                const std::string nowTemplate = const_suggestion::itemOf(now.value, "template:");
                return wasTemplate.empty() != nowTemplate.empty() || wasTemplate != nowTemplate;
            };
            FactComparison out;
            const auto compare = [&](const std::vector<Fact>& was, const std::vector<Fact>& now)
            {
                std::vector<FactKey> order; // as the traversal first meets each key
                std::map<FactKey, std::pair<std::vector<const Fact*>, std::vector<const Fact*>>> at;
                const auto add = [&](const FactKey& key, const Fact& fact, bool rewritten)
                {
                    auto [it, added] = at.try_emplace(key);
                    if (added)
                        order.push_back(key);
                    (rewritten ? it->second.second : it->second.first).push_back(&fact);
                };
                for (const Fact& fact : was)
                {
                    if (const std::optional<FactKey> key = forward(fact.key))
                        add(*key, fact, false);
                }
                for (const Fact& fact : now)
                {
                    if (known(fact.key))
                        add(fact.key, fact, true);
                }
                for (const FactKey& key : order)
                {
                    const auto& [wasAt, nowAt] = at.at(key);
                    if (wasAt.size() != nowAt.size())
                    {
                        const Fact& one = wasAt.empty() ? *nowAt.front() : *wasAt.front();
                        if (!out.incomplete)
                        {
                            out.incomplete =
                                "the facts at " + one.key.site.expansion.file + ":" +
                                std::to_string(one.line) +
                                " cannot be compared one to one: " + std::to_string(wasAt.size()) +
                                " before the rewrite, " + std::to_string(nowAt.size()) +
                                " after it";
                        }
                        continue;
                    }
                    for (std::size_t k = 0; k < wasAt.size(); ++k)
                    {
                        if (changed(*wasAt[k], *nowAt[k]))
                        {
                            out.changed = changeOf(*wasAt[k], *nowAt[k]);
                            return;
                        }
                    }
                }
            };
            compare(before.facts, after.facts);
            if (!out.changed && index < before.entities.size())
            {
                compare(before.entities[index].overrides,
                        entity ? entity->overrides : std::vector<Fact>());
            }
            return out;
        }

        // Step 3 (§5.6): a declaration that a rewritten file writes outside the plan and whose
        // type the rewrite changes, valid or not after it; and else, when a site's declarations
        // cannot be compared one to one, why the comparison is incomplete.
        std::optional<std::string> outsideThePlan(const UnitFacts& before, const UnitFacts& after,
                                                  const SuggestionPlan& plan)
        {
            using Declarations = std::vector<UnitFacts::Written>;
            const auto planned = [&](const DeclarationSite& site)
            {
                return !plan.edits.count(site.expansion.file) ||
                       std::binary_search(plan.entity.redeclarations.begin(),
                                          plan.entity.redeclarations.end(), site) ||
                       std::binary_search(plan.parameters.begin(), plan.parameters.end(), site);
            };
            std::optional<std::string> incomplete;
            const Declarations none;
            const auto compare = [&](const DeclarationSite& site, const Declarations& was,
                                     const Declarations& now) -> std::optional<std::string>
            {
                if (was.size() != now.size())
                {
                    const UnitFacts::Written& one = was.empty() ? now.front() : was.front();
                    if (!incomplete)
                    {
                        incomplete = "the declarations at " + site.expansion.file + ":" +
                                     std::to_string(one.line) + " cannot be compared one to one: " +
                                     std::to_string(was.size()) + " before the rewrite, " +
                                     std::to_string(now.size()) + " after it";
                    }
                    return std::nullopt;
                }
                for (std::size_t k = 0; k < was.size(); ++k)
                {
                    if (was[k].type != now[k].type)
                    {
                        return "the rewrite also changes the type of " + now[k].name + " (" +
                               site.expansion.file + ":" + std::to_string(now[k].line) + ")";
                    }
                }
                return std::nullopt;
            };
            for (const auto& [site, now] : after.writtenTypes)
            {
                if (planned(site))
                    continue;
                const auto was = before.writtenTypes.find(site);
                if (std::optional<std::string> changed =
                        compare(site, was == before.writtenTypes.end() ? none : was->second, now))
                {
                    return changed;
                }
            }
            for (const auto& [site, was] : before.writtenTypes)
            {
                if (planned(site) || after.writtenTypes.count(site))
                    continue;
                if (std::optional<std::string> changed = compare(site, was, none))
                    return changed;
            }
            // A type that an edit writes outside the planned parameters' types (D19).
            if (after.writtenOutside)
            {
                return "the rewrite also changes the type written at " +
                       after.writtenOutside->site.expansion.file + ":" +
                       std::to_string(after.writtenOutside->line);
            }
            return incomplete;
        }

        Judgement refuted(std::string why)
        {
            return {std::move(why), Verdict::Refuted};
        }

        Judgement indeterminate(std::string why)
        {
            return {std::move(why), Verdict::Indeterminate};
        }

        // The rule of a suggestion, as its diagnostic names it.
        std::string ruleOf(const ConstParamIssue& issue)
        {
            const char* rule = "Pointer";
            if (issue.pointerConstOnly)
                rule = "PointerConstOnly";
            else if (issue.isReference)
                rule = issue.isRvalueRef ? "ReferenceRvaluePreferValue" : "Reference";
            return std::string("ConstParameterNotModified.") + rule;
        }

        // Checks one suggestion, the target at `index`, in one unit, against the unit before the
        // rewrite: the judgement of §5.8. `after` receives the unit's compilation after the
        // rewrite, with the collisions it finds (§5.10); `definingFile` names the defining unit.
        Judgement checkUnit(const UnitCommand& command, const std::optional<Original>& before,
                            std::size_t index, const Target& planned,
                            const std::map<std::string, std::string>& rewritten,
                            const SuggestionPlan& plan, const std::string& definingFile,
                            PrintedSignature& signature, std::vector<std::string>& accepted,
                            std::string& renamed, UnitFacts& after, bool timing)
        {
            // Step 1: a unit that cannot be checked.
            if (!before)
                return indeterminate(kDoesNotCompileAsItIs);
            // Each declaration's candidate, as the unit writes it before the rewrite (D8, D18).
            Target target = planned;
            target.expected = before->unit.entities[index].expected;
            after = inspect(command, &plan, rewritten, {target}, "const_suggestion.compile.after",
                            timing);
            if (!after.failure.empty())
                return indeterminate(after.failure);
            // Step 2: after a fatal error, the rest of the unit is not inspected.
            if (!after.fatal.empty())
            {
                return indeterminate("the rewritten code cannot be inspected entirely, a fatal "
                                     "error stops its compilation: " +
                                     after.fatal);
            }
            const UnitFacts::Entity& facts =
                after.entities.empty() ? UnitFacts::Entity() : after.entities[0];
            // Step 3, the plan's scope (§5.6): its parameters have the candidate, and every other
            // declaration that a rewritten file writes keeps its type.
            if (facts.withoutCandidate)
                return indeterminate("the rewritten declaration does not get the suggested type");
            if (const std::optional<std::string> outside =
                    outsideThePlan(before->unit, after, plan))
                return indeterminate(*outside);
            // Steps 4 and 5: the first error or checked warning that the rewrite adds.
            for (const Diagnostic& diagnostic : after.reported)
            {
                if (after.diagnostics.count(diagnostic.key) >
                    before->unit.diagnostics.count(diagnostic.key))
                {
                    return refuted(
                        (diagnostic.error ? "does not compile: " : "gains a checked warning: ") +
                        diagnostic.text);
                }
            }
            // A rewrite that does not compile, and adds no error to tell why: it is not shown to be
            // wrong (#239 review, M6).
            if (!after.module)
            {
                return indeterminate(
                    "the rewritten code does not compile, and no error that the rewrite adds "
                    "explains it" +
                    (after.firstDiagnostic.empty() ? "" : ": " + after.firstDiagnostic));
            }
            // Another entity's declaration that the rewrite merges into the entity: a declaration
            // it has after the rewrite only (ruling 3).
            if (!after.entities.empty() && index < before->unit.entities.size())
            {
                UnitFacts::Entity& entity = after.entities[0];
                for (const auto& [site, line] : entity.declarations)
                {
                    if (!before->unit.entities[index].declarations.count(site))
                    {
                        entity.collisions.push_back(
                            {entity.variants.empty() ? planned.symbol : entity.variants[0].symbol,
                             site, std::nullopt, line});
                    }
                }
            }
            // The correspondences of the rewrite (§5.7), from the facts of both compilations; the
            // ABI line names the suggestion's own symbol after it.
            std::string missing;
            const std::vector<SymbolPair> pairs =
                const_suggestion::correspond(before->unit, index, after, missing);
            for (const SymbolPair& pair : pairs)
            {
                if (renamed.empty() && pair.before == planned.symbol)
                    renamed = pair.after;
            }
            // Step 6: a reference, an override or a constant condition that changes, compared
            // through the pairs; facts that cannot be compared one to one leave the unit
            // unverified, and so does a correspondence that is not one to one.
            const FactComparison compared = compareFacts(before->unit, index, after, pairs);
            if (compared.changed)
                return refuted(*compared.changed);
            if (compared.incomplete)
                return indeterminate(*compared.incomplete);
            // A unit that declares the entity for the linker only, with its current type: whether
            // that declaration matters depends on how the files are linked (D17).
            if (facts.stale)
            {
                return indeterminate(after.file + " declares " +
                                     formatFunctionNameForMessage(planned.symbol) +
                                     " with its current type and does not see the rewrite: if " +
                                     definingFile + " and " + after.file +
                                     " are linked together, that declaration no longer matches "
                                     "the rewritten definition");
            }
            // An explicit cast of its address, other than to void: what it hides may change with
            // the rewrite, and neither the errors nor the facts tell (D15). Alone, it leaves the
            // unit unverified; the steps above still refute.
            if (!facts.casts.empty())
            {
                const UnitFacts::Entity::Cast& cast = facts.casts.front();
                return indeterminate(cast.site.expansion.file + ":" + std::to_string(cast.line) +
                                     " converts the address of " +
                                     formatFunctionNameForMessage(planned.symbol) +
                                     " with an explicit cast");
            }
            if (!missing.empty())
                return indeterminate(missing);
            if (signature.after.empty() && !facts.printedSignature.empty())
                signature.after = facts.printedSignature;
            // Step 7: the generated code, compared through the pairs. A difference that no
            // reference, override or constant condition explains leaves the unit unverified: it
            // never refutes.
            std::optional<std::string> difference;
            {
                const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.compare");
                const const_suggestion::ModuleText now = const_suggestion::prepare(*after.module);
                // A renamed symbol that this file only declares, on both sides, other than the
                // entity's own variants and inheriting constructors (which the defining file
                // compiles): comparing its two declarations proves nothing about the code that
                // another file defines for it, if any does.
                std::set<std::string> own;
                if (index < before->unit.entities.size())
                {
                    for (const const_suggestion::Named& named :
                         before->unit.entities[index].variants)
                        own.insert(named.symbol);
                    for (const const_suggestion::Named& named :
                         before->unit.entities[index].inheriting)
                        own.insert(named.symbol);
                }
                for (const SymbolPair& pair : pairs)
                {
                    if (own.count(pair.before) || !before->code->globals.count(pair.before) ||
                        !now.globals.count(pair.after) ||
                        before->code->defined.count(pair.before) || now.defined.count(pair.after))
                    {
                        continue;
                    }
                    return indeterminate(formatFunctionNameForMessage(pair.before) + " becomes " +
                                         formatFunctionNameForMessage(pair.after) + ", which " +
                                         after.file +
                                         " only declares: the code another file defines for it "
                                         "is not checked");
                }
                difference = const_suggestion::codeDifference(*before->code, now, pairs, &signature,
                                                              &accepted);
            }
            if (difference)
            {
                return indeterminate("the generated code changes, and no reference, override or "
                                     "constant condition explains it: " +
                                     *difference);
            }
            return {};
        }
    } // namespace

    std::string const_suggestion::fingerprintOf(const UnitCommand& command)
    {
        std::string text;
        for (const std::string& arg : command.args)
            text.append(arg).push_back('\0');
        text.append(command.workingDir).push_back('\0');
        return llvm::utohexstr(llvm::xxh3_64bits(text), /*LowerCase=*/true, /*Width=*/16);
    }

    std::string const_suggestion::realPath(llvm::StringRef path)
    {
        llvm::SmallString<256> out;
        if (llvm::sys::fs::real_path(path, out))
            return path.str();
        return out.str().str();
    }

    Judgement const_suggestion::aggregate(const std::vector<Judgement>& judgements)
    {
        for (const Verdict verdict : {Verdict::Refuted, Verdict::Indeterminate})
        {
            for (const Judgement& judgement : judgements)
            {
                if (judgement.verdict == verdict)
                    return judgement;
            }
        }
        return {};
    }

    std::shared_ptr<const AnalyzedUnits> analyzedUnitOf(const std::string& filename,
                                                        const llvm::Module& module)
    {
        auto units = std::make_shared<AnalyzedUnits>();
        units->units.push_back({filename, &module, detectFromExtension(filename)});
        return units;
    }

    void checkConstSuggestions(const llvm::Module& mod, std::vector<ConstParamIssue>& issues,
                               const AnalysisConfig& config)
    {
        if (issues.empty() || config.warningsOnly || !config.constSuggestionCheck)
            return;
        const bool timing = config.timing;
        const analyzer::ScopedHotspot total(timing, "const_suggestion.total");
        const AnalyzedUnits* units = config.analyzedUnits.get();
        const AnalyzedUnit* self = nullptr;
        for (std::size_t i = 0; units && i < units->units.size(); ++i)
        {
            if (units->units[i].module == &mod)
                self = &units->units[i];
        }
        const auto trace = [](const ConstParamIssue& issue, const std::string& outcome)
        {
            coretrace::log(coretrace::Level::Debug, "[const-suggestion] {} parameter {}: {}\n",
                           issue.funcName, issue.binding.name, outcome);
        };
        // The verdict's grounds, after T v1's line (§4): the suggestion, the type it tries, where
        // the rewrite writes it, each unit judged with its configuration, then the verdict. A
        // suggestion whose plan is not executed has no rewrite and no unit.
        const auto grounds = [](const ConstParamIssue& issue, const SuggestionPlan* plan,
                                const std::vector<std::string>& judged,
                                const std::vector<Judgement>& judgements, const Judgement& verdict)
        {
            static constexpr const char* kVerdicts[] = {"verified", "refuted", "indeterminate"};
            static const std::map<std::string, std::vector<Edit>> kNoEdits;
            const auto line = [&](const std::string& text)
            {
                coretrace::log(coretrace::Level::Debug, "[const-suggestion] {} parameter {}, {}\n",
                               issue.funcName, issue.binding.name, text);
            };
            const auto outcome = [](const Judgement& judgement)
            {
                return kVerdicts[static_cast<std::size_t>(judgement.verdict)] +
                       (judgement.reason.empty() ? std::string() : ": " + judgement.reason);
            };
            line("suggestion: " + ruleOf(issue) + " on " +
                 formatFunctionNameForMessage(issue.funcName) + ", source index " +
                 (issue.binding.sourceIndex ? std::to_string(*issue.binding.sourceIndex) : "none"));
            if (plan && !plan->candidate.type.empty())
            {
                line("transformation: " + plan->candidate.current + " becomes " +
                     plan->candidate.type + " (" + plan->candidate.alternative + ")");
            }
            for (const auto& [path, edits] : plan && !plan->indeterminate ? plan->edits : kNoEdits)
            {
                std::vector<Edit> sorted = edits;
                std::sort(sorted.begin(), sorted.end(),
                          [](const Edit& a, const Edit& b) { return a.offset < b.offset; });
                for (const Edit& edit : sorted)
                    line("rewritten at: " + path + ":" + std::to_string(edit.line));
            }
            for (std::size_t k = 0; k < judgements.size(); ++k)
                line("unit " + judged[k] + ": " + outcome(judgements[k]));
            line("verdict: " + outcome(verdict));
        };
        const auto label = [](const std::string& file, const UnitCommand* command)
        {
            return file + " [" + (command ? const_suggestion::fingerprintOf(*command) : "none") +
                   "]";
        };
        const auto notChecked = [&](const std::string& why)
        {
            for (ConstParamIssue& issue : issues)
            {
                issue.checkLines.push_back(kNotChecked + why);
                trace(issue, "not checked, " + why);
                grounds(issue, nullptr, {}, {}, indeterminate(why));
            }
        };
        if (!self || (self->language != LanguageType::C && self->language != LanguageType::CXX))
        {
            notChecked("no source file, so whether the suggested type compiles was not verified");
            return;
        }
        UnitCommand command;
        std::string error;
        if (!commandFor(*self, config, command, error))
        {
            notChecked("no compile command (" + error + ")");
            return;
        }

        // Before the rewrite, the defining unit: one compilation plans every suggestion.
        std::vector<Target> targets(issues.size());
        for (std::size_t i = 0; i < issues.size(); ++i)
        {
            targets[i].symbol = issues[i].funcName;
            if (!issues[i].binding.isArtificial)
                targets[i].sourceIndex = issues[i].binding.sourceIndex;
        }
        UnitFacts inspected = inspect(command, nullptr, {}, targets,
                                      "const_suggestion.compile.defining_before", timing);
        if (!inspected.failure.empty() || !inspected.module ||
            inspected.entities.size() != issues.size())
        {
            notChecked("the file does not compile with Clang as it is");
            return;
        }
        const std::optional<Original> definingBefore = originalFrom(std::move(inspected), timing);
        const UnitFacts& before = definingBefore->unit;
        // The suggestions as planned: what the other compilations look for.
        for (std::size_t i = 0; i < issues.size(); ++i)
            targets[i].plan = &before.entities[i].plan;
        OriginalCache originals;

        // The other units a rewrite may reach: the analyzed ones and, with a compilation
        // database, every C or C++ file it lists.
        std::vector<AnalyzedUnit> candidates;
        std::set<std::string> analyzed;
        for (const AnalyzedUnit& unit : units->units)
        {
            analyzed.insert(canonicalPath(unit.filename, config));
            if (&unit != self)
                candidates.push_back(unit);
        }
        if (config.compilationDatabase)
        {
            for (const std::string& file : config.compilationDatabase->listSourceFiles())
            {
                const LanguageType language = detectFromExtension(file);
                if ((language == LanguageType::C || language == LanguageType::CXX) &&
                    !analyzed.count(canonicalPath(file, config)))
                {
                    candidates.push_back({file, nullptr, language});
                }
            }
        }

        std::vector<ConstParamIssue> kept;
        for (std::size_t i = 0; i < issues.size(); ++i)
        {
            ConstParamIssue& issue = issues[i];
            const UnitFacts::Entity& found = before.entities[i];
            const SuggestionPlan& plan = found.plan;
            // The type displayed is the plan's candidate, not the rule's proposal (stage 2,
            // ruling 3): for an rvalue reference, the const reference alone.
            if (!plan.candidate.shown.empty() && (issue.isRvalueRef || issue.pointerConstOnly))
            {
                issue.suggestedType = plan.candidate.shown;
                issue.suggestedTypeAlt.clear();
            }
            if (!found.found && !plan.indeterminate)
            {
                issue.checkLines.push_back(kNotChecked +
                                           std::string("the function is not found in the source"));
                trace(issue, "not checked, the function is not found in the source");
                grounds(issue, &plan, {}, {},
                        indeterminate("the function is not found in the source"));
                kept.push_back(std::move(issue));
                continue;
            }
            if (plan.indeterminate)
            {
                issue.checkLines.push_back(kNotChecked + *plan.indeterminate);
                trace(issue, "not checked, " + *plan.indeterminate);
                grounds(issue, &plan, {}, {}, indeterminate(*plan.indeterminate));
                kept.push_back(std::move(issue));
                continue;
            }
            std::string unreadable;
            const std::map<std::string, std::string> rewritten =
                const_suggestion::rewrite(plan, unreadable);
            if (!unreadable.empty())
            {
                issue.checkLines.push_back(kNotChecked + unreadable);
                trace(issue, "not checked, " + unreadable);
                grounds(issue, &plan, {}, {}, indeterminate(unreadable));
                kept.push_back(std::move(issue));
                continue;
            }
            std::set<std::string> rewrittenFiles;
            for (const auto& [path, text] : rewritten)
                rewrittenFiles.insert(path);

            // The defining unit first: it gives the function's symbol after the rewrite.
            const Target& planned = targets[i];
            PrintedSignature signature{found.printedSignature, ""};
            std::vector<std::string> accepted;
            std::string renamed;
            // Each unit judged, as its compilation after the rewrite shows it: its collisions
            // are judged once none of the units refutes (§5.10). Its module is not kept.
            std::vector<UnitFacts> inspected(1);
            const auto release = [](UnitFacts& unit)
            {
                unit.module.reset();
                unit.context.reset();
            };
            const std::string definingFile = canonicalPath(self->filename, config);
            std::vector<Judgement> judgements{
                checkUnit(command, definingBefore, i, planned, rewritten, plan, definingFile,
                          signature, accepted, renamed, inspected.back(), timing)};
            release(inspected.back());
            std::vector<std::string> judged{label(definingFile, &command)};
            // The other units look for the symbols the entity takes after the rewrite, as the
            // defining unit names them.
            Target others = planned;
            if (!inspected.back().entities.empty())
            {
                for (const const_suggestion::Named& variant : inspected.back().entities[0].variants)
                    others.takes.push_back(variant.symbol);
            }
            // Then every other unit that sees a rewritten file or references the function: the
            // analyzed ones and, with a compilation database, every file it lists, analyzed or
            // not. One that is not verified, the defining one included, leaves the suggestion
            // unchecked, unless another one refutes it: only a refutation stops the judgement
            // (§5.10).
            const auto cannotCheck =
                [&](const std::string& unit, const UnitCommand* known, const std::string& why)
            {
                judged.push_back(label(unit, known));
                judgements.push_back(indeterminate(why));
            };
            for (const AnalyzedUnit& unit : candidates)
            {
                if (judgements.back().verdict == Verdict::Refuted)
                    break;
                // Another file references the function only through the linker: one with
                // internal linkage is another file's homonym (§5.2).
                const bool referencing = plan.entity.linkage == SourceLinkage::External &&
                                         unit.module && references(*unit.module, issue.funcName);
                if (unit.language != LanguageType::C && unit.language != LanguageType::CXX)
                {
                    if (referencing)
                    {
                        cannotCheck(canonicalPath(unit.filename, config), nullptr,
                                    unit.filename + " uses the function but has no source");
                    }
                    continue;
                }
                const std::string key = canonicalPath(unit.filename, config);
                std::optional<UnitCommand> known;
                bool resolved = false;
                {
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    const auto found = units->commands.find(key);
                    if (found != units->commands.end())
                    {
                        known = found->second;
                        resolved = true;
                    }
                }
                if (!resolved)
                {
                    UnitCommand command;
                    if (commandFor(unit, config, command, error))
                        known = std::move(command);
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    units->commands[key] = known;
                }
                if (!known)
                {
                    cannotCheck(key, nullptr, "no compile command for " + unit.filename);
                    continue;
                }
                const UnitCommand& other = *known;
                UnitScan seen;
                bool cached = false;
                {
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    const auto found = units->scans.find(key);
                    if (found != units->scans.end())
                    {
                        seen = found->second;
                        cached = true;
                    }
                }
                if (!cached)
                {
                    seen = scan(other, timing);
                    std::lock_guard<std::mutex> lock(units->scansMutex);
                    units->scans[key] = seen;
                }
                if (seen.files.empty())
                {
                    cannotCheck(key, &other, unit.filename + " cannot be preprocessed with Clang");
                    continue;
                }
                const bool sees =
                    referencing ||
                    std::any_of(rewrittenFiles.begin(), rewrittenFiles.end(),
                                [&](const std::string& path) { return seen.files.count(path); }) ||
                    (plan.declarableElsewhere &&
                     seen.identifiers.count(llvm::xxh3_64bits(
                         plan.entity.operatorName ? llvm::StringRef("operator")
                                                  : llvm::StringRef(plan.entity.name))) != 0);
                if (!sees)
                    continue;
                std::vector<std::string> otherAccepted;
                Judgement judgement =
                    checkUnit(other, originalOf(other, key, targets, originals, timing), i, others,
                              rewritten, plan, definingFile, signature, otherAccepted, renamed,
                              inspected.emplace_back(), timing);
                release(inspected.back());
                if (judgement.verdict == Verdict::Indeterminate &&
                    judgement.reason == kDoesNotCompileAsItIs)
                {
                    judgement.reason = unit.filename + " does not compile with Clang as it is";
                }
                judged.push_back(label(key, &other));
                judgements.push_back(std::move(judgement));
                for (const std::string& line : otherAccepted)
                {
                    if (std::find(accepted.begin(), accepted.end(), line) == accepted.end())
                        accepted.push_back(line);
                }
            }
            Judgement verdict = const_suggestion::aggregate(judgements);
            if (verdict.verdict == Verdict::Verified)
            {
                std::vector<const UnitFacts*> units;
                for (const UnitFacts& unit : inspected)
                    units.push_back(&unit);
                verdict = const_suggestion::judgeCollisions(
                    units, 0, definingFile, formatFunctionNameForMessage(issue.funcName));
            }
            if (verdict.verdict == Verdict::Refuted)
            {
                trace(issue, "removed, " + verdict.reason);
                grounds(issue, &plan, judged, judgements, verdict);
                continue;
            }
            if (verdict.verdict == Verdict::Indeterminate)
            {
                issue.checkLines.push_back(kNotChecked + verdict.reason);
                trace(issue, "not checked, " + verdict.reason);
                grounds(issue, &plan, judged, judgements, verdict);
                kept.push_back(std::move(issue));
                continue;
            }
            trace(issue, "checked in " + std::to_string(judgements.size()) + " file(s)");
            grounds(issue, &plan, judged, judgements, verdict);
            const std::string checked =
                std::string(config.compilationDatabase ? kCheckedDatabase : kCheckedAnalyzed) +
                (issue.isRvalueRef ? kCheckedReference : kCheckedResult);
            issue.checkLines.push_back(
                accepted.empty() ? checked : checked + ", except for the differences below");
            issue.checkLines.insert(issue.checkLines.end(), accepted.begin(), accepted.end());
            const llvm::Function* function = mod.getFunction(issue.funcName);
            if (function && function->hasExternalLinkage() && !renamed.empty() &&
                renamed != issue.funcName)
            {
                issue.checkLines.push_back("ABI: the suggested type changes the symbol (" +
                                           issue.funcName + " -> " + renamed +
                                           "): code compiled separately must use the updated "
                                           "declaration and be recompiled");
            }
            kept.push_back(std::move(issue));
        }
        issues = std::move(kept);
    }
} // namespace ctrace::stack::analysis
