// SPDX-License-Identifier: Apache-2.0
// Checks the const suggestions of ConstParameterNotModified with Clang (#217): the analysis of the
// IR decides whether an execution writes the pointee; Clang decides whether the suggested type
// compiles in every analyzed file and leaves the generated code as it is.
#include "analysis/ConstSuggestionCheck.hpp"

#include "ConstSuggestionInternal.hpp"
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
        using const_suggestion::Diagnostic;
        using const_suggestion::Edit;
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
        constexpr const char* kNotChecked = "not checked with Clang: ";
        constexpr const char* kDoesNotCompileAsItIs = "a file does not compile with Clang as it is";

        // A unit before the rewrite, prepared once for every suggestion of the module: its
        // diagnostics, what the methods of each function's name override (indexed like the
        // targets), and its generated code ready to compare.
        struct Original
        {
            std::multiset<std::string> diagnostics;
            std::vector<std::multiset<std::string>> overrides;
            std::shared_ptr<const const_suggestion::ModuleText> code;
        };

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

        std::map<std::string, std::string> rewrite(const SuggestionPlan& plan)
        {
            std::map<std::string, std::string> out;
            for (const auto& [path, edits] : plan.edits)
            {
                llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
                    llvm::MemoryBuffer::getFile(path);
                if (!buffer)
                    continue;
                std::string text = (*buffer)->getBuffer().str();
                std::vector<Edit> sorted = edits;
                std::sort(sorted.begin(), sorted.end(),
                          [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
                for (const Edit& edit : sorted)
                    text.replace(edit.offset, edit.length, edit.text);
                out[path] = std::move(text);
            }
            return out;
        }

        // The overrides that each target's inspection recorded, indexed like the targets.
        std::vector<std::multiset<std::string>> overridesOf(const UnitFacts& unit)
        {
            std::vector<std::multiset<std::string>> out;
            for (const UnitFacts::Entity& entity : unit.entities)
                out.push_back(entity.overrides);
            return out;
        }

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
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.prepare");
            out = Original{before.diagnostics, overridesOf(before),
                           std::make_shared<const_suggestion::ModuleText>(
                               const_suggestion::prepare(*before.module))};
            return out;
        }

        Judgement refuted(std::string why)
        {
            return {std::move(why), Verdict::Refuted};
        }

        Judgement indeterminate(std::string why)
        {
            return {std::move(why), Verdict::Indeterminate};
        }

        // Checks one suggestion, the target at `index`, in one unit, against the unit before the
        // rewrite.
        Judgement checkUnit(const UnitCommand& command, const std::optional<Original>& before,
                            std::size_t index, const Target& planned,
                            const std::map<std::string, std::string>& rewritten,
                            const SuggestionPlan& plan, PrintedSignature& signature,
                            std::vector<std::string>& accepted, std::string& renamed, bool timing)
        {
            if (!before)
                return indeterminate(kDoesNotCompileAsItIs);
            UnitFacts after = inspect(command, &plan, rewritten, {planned},
                                      "const_suggestion.compile.after", timing);
            if (!after.failure.empty())
                return indeterminate(after.failure);
            // The first diagnostic the rewrite adds, not one the unit had before it.
            for (const Diagnostic& diagnostic : after.reported)
            {
                if (after.diagnostics.count(diagnostic.key) >
                    before->diagnostics.count(diagnostic.key))
                {
                    return refuted(
                        (diagnostic.error ? "does not compile: " : "gains a checked warning: ") +
                        diagnostic.text);
                }
            }
            if (!after.module)
                return refuted("does not compile: " + after.firstDiagnostic);
            const UnitFacts::Entity& facts =
                after.entities.empty() ? UnitFacts::Entity() : after.entities[0];
            // The suggestion's variant after the rewrite.
            const auto variant =
                std::find(plan.entity.symbols.begin(), plan.entity.symbols.end(), planned.symbol);
            const auto k = static_cast<std::size_t>(variant - plan.entity.symbols.begin());
            if (renamed.empty() && k < facts.symbols.size())
                renamed = facts.symbols[k];
            if (facts.problem.starts_with("rewritten:"))
                return indeterminate("the rewritten declaration does not get the suggested type");
            if (!facts.problem.empty())
                return refuted(facts.problem);
            if (before->overrides[index] != facts.overrides)
                return refuted("the methods it overrides change");
            if (signature.after.empty() && !facts.printedSignature.empty())
                signature.after = facts.printedSignature;
            std::string renamedTo;
            std::optional<std::string> difference;
            {
                const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.compare");
                const const_suggestion::ModuleText now = const_suggestion::prepare(*after.module);
                difference = const_suggestion::codeDifference(*before->code, now, planned.symbol,
                                                              &signature, &accepted, &renamedTo);
            }
            if (difference)
                return refuted("the generated code changes: " + *difference);
            if (renamed.empty() || renamed == planned.symbol)
                renamed = renamedTo;
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
        // Each unit judged, by its file and its configuration, after T v1's line.
        const auto traceUnits = [](const ConstParamIssue& issue,
                                   const std::vector<std::string>& judged,
                                   const std::vector<Judgement>& judgements)
        {
            static constexpr const char* kVerdicts[] = {"verified", "refuted", "indeterminate"};
            for (std::size_t k = 0; k < judgements.size(); ++k)
            {
                const Judgement& judgement = judgements[k];
                coretrace::log(coretrace::Level::Debug,
                               "[const-suggestion] {} parameter {}, unit {}: {}{}\n",
                               issue.funcName, issue.binding.name, judged[k],
                               kVerdicts[static_cast<std::size_t>(judgement.verdict)],
                               judgement.reason.empty() ? "" : ": " + judgement.reason);
            }
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
        UnitFacts before = inspect(command, nullptr, {}, targets,
                                   "const_suggestion.compile.defining_before", timing);
        if (!before.failure.empty() || !before.module || before.entities.size() != issues.size())
        {
            notChecked("the file does not compile with Clang as it is");
            return;
        }
        // The suggestions as planned: what the other compilations look for.
        for (std::size_t i = 0; i < issues.size(); ++i)
            targets[i].plan = &before.entities[i].plan;
        std::shared_ptr<const const_suggestion::ModuleText> definingCode;
        {
            const analyzer::ScopedHotspot hotspot(timing, "const_suggestion.prepare");
            definingCode = std::make_shared<const_suggestion::ModuleText>(
                const_suggestion::prepare(*before.module));
        }
        const std::optional<Original> definingBefore =
            Original{before.diagnostics, overridesOf(before), definingCode};
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
            if (!found.found && !plan.indeterminate)
            {
                issue.checkLines.push_back(kNotChecked +
                                           std::string("the function is not found in the source"));
                trace(issue, "not checked, the function is not found in the source");
                kept.push_back(std::move(issue));
                continue;
            }
            if (plan.indeterminate)
            {
                issue.checkLines.push_back(kNotChecked + *plan.indeterminate);
                trace(issue, "not checked, " + *plan.indeterminate);
                kept.push_back(std::move(issue));
                continue;
            }
            const std::map<std::string, std::string> rewritten = rewrite(plan);
            std::set<std::string> rewrittenFiles;
            for (const auto& [path, text] : rewritten)
                rewrittenFiles.insert(path);

            // The defining unit first: it gives the function's symbol after the rewrite.
            const Target& planned = targets[i];
            PrintedSignature signature{found.printedSignature, ""};
            std::vector<std::string> accepted;
            std::string renamed;
            std::vector<Judgement> judgements{checkUnit(command, definingBefore, i, planned,
                                                        rewritten, plan, signature, accepted,
                                                        renamed, timing)};
            std::vector<std::string> judged{label(canonicalPath(self->filename, config), &command)};
            // Then every other unit that sees a rewritten file or references the function: the
            // analyzed ones and, with a compilation database, every file it lists, analyzed or
            // not. One that cannot be checked leaves the suggestion unchecked, unless another
            // one refutes it.
            const auto cannotCheck =
                [&](const std::string& unit, const UnitCommand* known, const std::string& why)
            {
                judged.push_back(label(unit, known));
                judgements.push_back(indeterminate(why));
            };
            for (const AnalyzedUnit& unit : candidates)
            {
                if (judgements.front().verdict != Verdict::Verified ||
                    judgements.back().verdict == Verdict::Refuted)
                {
                    break;
                }
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
                     seen.identifiers.count(llvm::xxh3_64bits(plan.entity.name)) != 0);
                if (!sees)
                    continue;
                std::vector<std::string> otherAccepted;
                Judgement judgement =
                    checkUnit(other, originalOf(other, key, targets, originals, timing), i, planned,
                              rewritten, plan, signature, otherAccepted, renamed, timing);
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
            const Judgement verdict = const_suggestion::aggregate(judgements);
            if (verdict.verdict == Verdict::Refuted)
            {
                trace(issue, "removed, " + verdict.reason);
                traceUnits(issue, judged, judgements);
                continue;
            }
            if (verdict.verdict == Verdict::Indeterminate)
            {
                issue.checkLines.push_back(kNotChecked + verdict.reason);
                trace(issue, "not checked, " + verdict.reason);
                traceUnits(issue, judged, judgements);
                kept.push_back(std::move(issue));
                continue;
            }
            trace(issue, "checked in " + std::to_string(judgements.size()) + " file(s)");
            traceUnits(issue, judged, judgements);
            const std::string checked =
                std::string(config.compilationDatabase ? kCheckedDatabase : kCheckedAnalyzed) +
                kCheckedResult;
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
