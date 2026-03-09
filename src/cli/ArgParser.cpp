#include "cli/ArgParser.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ctrace::stack::cli
{
    namespace
    {
        struct OptionCandidate
        {
            std::string_view matcher;
            std::string_view suggestion;
        };

        class UnknownOptionSuggester
        {
          public:
            static std::optional<std::string> suggest(std::string_view unknownOption)
            {
                if (unknownOption.empty() || unknownOption.front() != '-')
                    return std::nullopt;

                if (isLongOptionWithInlineValue(unknownOption))
                {
                    if (auto fixedValueSuggestion = suggestFixedValueOption(unknownOption))
                        return fixedValueSuggestion;
                    return suggestLongOptionBase(unknownOption);
                }

                return suggestByMatcher(unknownOption, false);
            }

          private:
            static constexpr std::array<OptionCandidate, 54> kCandidates = {
                {{"-h", "-h"},
                 {"--help", "--help"},
                 {"--demangle", "--demangle"},
                 {"--quiet", "--quiet"},
                 {"--verbose", "--verbose"},
                 {"--stl", "--STL"},
                 {"--only-file", "--only-file"},
                 {"--only-func", "--only-func"},
                 {"--only-function", "--only-function"},
                 {"--only-dir", "--only-dir"},
                 {"--exclude-dir", "--exclude-dir"},
                 {"--stack-limit", "--stack-limit"},
                 {"--dump-filter", "--dump-filter"},
                 {"--dump-ir", "--dump-ir"},
                 {"-I", "-I"},
                 {"-D", "-D"},
                 {"--compile-arg", "--compile-arg"},
                 {"--compdb-fast", "--compdb-fast"},
                 {"--analysis-profile", "--analysis-profile"},
                 {"--include-compdb-deps", "--include-compdb-deps"},
                 {"--jobs", "--jobs"},
                 {"--timing", "--timing"},
                 {"--smt", "--smt=on"},
                 {"--smt=on", "--smt=on"},
                 {"--smt=off", "--smt=off"},
                 {"--smt-backend", "--smt-backend=interval"},
                 {"--smt-secondary-backend", "--smt-secondary-backend=interval"},
                 {"--smt-mode", "--smt-mode=single"},
                 {"--smt-timeout-ms", "--smt-timeout-ms"},
                 {"--smt-budget-nodes", "--smt-budget-nodes"},
                 {"--smt-rules", "--smt-rules"},
                 {"--resource-model", "--resource-model"},
                 {"--escape-model", "--escape-model"},
                 {"--buffer-model", "--buffer-model"},
                 {"--resource-cross-tu", "--resource-cross-tu"},
                 {"--no-resource-cross-tu", "--no-resource-cross-tu"},
                 {"--uninitialized-cross-tu", "--uninitialized-cross-tu"},
                 {"--no-uninitialized-cross-tu", "--no-uninitialized-cross-tu"},
                 {"--resource-summary-cache-dir", "--resource-summary-cache-dir"},
                 {"--resource-summary-cache-memory-only", "--resource-summary-cache-memory-only"},
                 {"--compile-ir-cache-dir", "--compile-ir-cache-dir"},
                 {"--config", "--config"},
                 {"--print-effective-config", "--print-effective-config"},
                 {"--compile-commands", "--compile-commands"},
                 {"--compdb", "--compdb"},
                 {"--warnings-only", "--warnings-only"},
                 {"--base-dir", "--base-dir"},
                 {"--format", "--format=json"},
                 {"--format=json", "--format=json"},
                 {"--format=sarif", "--format=sarif"},
                 {"--format=human", "--format=human"},
                 {"--mode", "--mode=ir"},
                 {"--mode=ir", "--mode=ir"},
                 {"--mode=abi", "--mode=abi"}}};

            struct CandidateScore
            {
                std::string_view suggestion;
                std::size_t distance = std::numeric_limits<std::size_t>::max();
                std::size_t queryLength = 0;
                std::uint64_t valid : 1 = false;
                std::uint64_t reservedFlags : 63 = 0;
            };

            static std::optional<std::string>
            suggestFixedValueOption(std::string_view unknownOption)
            {
                return suggestByMatcher(unknownOption, true);
            }

            static std::optional<std::string> suggestLongOptionBase(std::string_view unknownOption)
            {
                const std::size_t eqPos = unknownOption.find('=');
                const std::string_view base = unknownOption.substr(0, eqPos);
                return suggestByMatcher(base, false);
            }

            static std::optional<std::string> suggestByMatcher(std::string_view query,
                                                               bool onlyFixedValueCandidates)
            {
                CandidateScore best = findBestCandidate(query, onlyFixedValueCandidates);
                if (!best.valid)
                    return std::nullopt;

                if (best.distance > maxAcceptedDistance(best.queryLength))
                    return std::nullopt;
                return std::string(best.suggestion);
            }

            static CandidateScore findBestCandidate(std::string_view query,
                                                    bool onlyFixedValueCandidates)
            {
                const std::string loweredQuery = toLowerCopy(query);
                CandidateScore best;
                if (loweredQuery.empty())
                    return best;

                for (const OptionCandidate& candidate : kCandidates)
                {
                    const bool isFixedValue = candidate.matcher.find('=') != std::string_view::npos;
                    if (onlyFixedValueCandidates && !isFixedValue)
                        continue;

                    const std::string loweredMatcher = toLowerCopy(candidate.matcher);
                    const std::size_t distance = levenshteinDistance(loweredQuery, loweredMatcher);

                    if (!best.valid || distance < best.distance ||
                        (distance == best.distance && candidate.suggestion < best.suggestion))
                    {
                        best.valid = true;
                        best.distance = distance;
                        best.suggestion = candidate.suggestion;
                        best.queryLength = loweredQuery.size();
                    }
                }
                return best;
            }

            static bool isLongOptionWithInlineValue(std::string_view option)
            {
                return option.size() > 2 && option[0] == '-' && option[1] == '-' &&
                       option.find('=') != std::string_view::npos;
            }

            static std::size_t maxAcceptedDistance(std::size_t queryLength)
            {
                if (queryLength <= 4)
                    return 1;
                if (queryLength <= 12)
                    return 2;
                return 3;
            }

            static std::string toLowerCopy(std::string_view input)
            {
                std::string lowered;
                lowered.reserve(input.size());
                for (char c : input)
                {
                    lowered.push_back(
                        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }
                return lowered;
            }

            static std::size_t levenshteinDistance(std::string_view lhs, std::string_view rhs)
            {
                if (lhs.empty())
                    return rhs.size();
                if (rhs.empty())
                    return lhs.size();

                std::vector<std::size_t> prev(rhs.size() + 1);
                std::vector<std::size_t> cur(rhs.size() + 1);
                for (std::size_t j = 0; j <= rhs.size(); ++j)
                    prev[j] = j;

                for (std::size_t i = 1; i <= lhs.size(); ++i)
                {
                    cur[0] = i;
                    for (std::size_t j = 1; j <= rhs.size(); ++j)
                    {
                        const std::size_t substitutionCost = (lhs[i - 1] == rhs[j - 1]) ? 0 : 1;
                        cur[j] =
                            std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + substitutionCost});
                    }
                    prev.swap(cur);
                }
                return prev[rhs.size()];
            }
        };

        std::string trimCopy(const std::string& input)
        {
            std::size_t start = 0;
            while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])))
                ++start;
            std::size_t end = input.size();
            while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
                --end;
            return input.substr(start, end - start);
        }

        bool parsePositiveUnsigned(const std::string& input, unsigned& out, std::string& error)
        {
            const std::string trimmed = trimCopy(input);
            if (trimmed.empty())
            {
                error = "value is empty";
                return false;
            }

            unsigned long long parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, 10);
            if (ec != std::errc() || ptr != trimmed.data() + trimmed.size())
            {
                error = "invalid numeric value";
                return false;
            }
            if (parsed == 0)
            {
                error = "value must be greater than zero";
                return false;
            }
            if (parsed > std::numeric_limits<unsigned>::max())
            {
                error = "value is too large";
                return false;
            }
            out = static_cast<unsigned>(parsed);
            return true;
        }

        bool parsePositiveU64(const std::string& input, std::uint64_t& out, std::string& error)
        {
            const std::string trimmed = trimCopy(input);
            if (trimmed.empty())
            {
                error = "value is empty";
                return false;
            }

            unsigned long long parsed = 0;
            const auto [ptr, ec] =
                std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, 10);
            if (ec != std::errc() || ptr != trimmed.data() + trimmed.size())
            {
                error = "invalid numeric value";
                return false;
            }
            if (parsed == 0)
            {
                error = "value must be greater than zero";
                return false;
            }
            out = static_cast<std::uint64_t>(parsed);
            return true;
        }

        bool parseJobsValue(const std::string& input, unsigned& jobsOut, bool& autoOut,
                            std::string& error)
        {
            const std::string trimmed = trimCopy(input);
            std::string lowered;
            lowered.reserve(trimmed.size());
            for (char c : trimmed)
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (lowered == "auto")
            {
                const unsigned hw = std::thread::hardware_concurrency();
                jobsOut = hw == 0 ? 1u : hw;
                autoOut = true;
                return true;
            }

            unsigned parsedJobs = 0;
            if (!parsePositiveUnsigned(trimmed, parsedJobs, error))
                return false;

            jobsOut = parsedJobs;
            autoOut = false;
            return true;
        }

        bool parseSmtSwitch(const std::string& input, bool& out, std::string& error)
        {
            std::string lowered;
            lowered.reserve(input.size());
            for (char c : trimCopy(input))
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (lowered == "on")
            {
                out = true;
                return true;
            }
            if (lowered == "off")
            {
                out = false;
                return true;
            }

            error = "expected 'on' or 'off'";
            return false;
        }

        bool parseSmtMode(const std::string& input, analysis::smt::SolverMode& out,
                          std::string& error)
        {
            std::string lowered;
            lowered.reserve(input.size());
            for (char c : trimCopy(input))
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (lowered == "single")
            {
                out = analysis::smt::SolverMode::Single;
                return true;
            }
            if (lowered == "portfolio")
            {
                out = analysis::smt::SolverMode::Portfolio;
                return true;
            }
            if (lowered == "cross-check")
            {
                out = analysis::smt::SolverMode::CrossCheck;
                return true;
            }
            if (lowered == "dual-consensus")
            {
                out = analysis::smt::SolverMode::DualConsensus;
                return true;
            }

            error = "expected 'single', 'portfolio', 'cross-check', or 'dual-consensus'";
            return false;
        }

        bool parseAnalysisProfile(const std::string& input, AnalysisProfile& out,
                                  std::string& error)
        {
            std::string trimmed = trimCopy(input);
            std::string lowered;
            lowered.reserve(trimmed.size());
            for (char c : trimmed)
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (lowered == "fast")
            {
                out = AnalysisProfile::Fast;
                return true;
            }
            if (lowered == "full")
            {
                out = AnalysisProfile::Full;
                return true;
            }
            error = "expected 'fast' or 'full'";
            return false;
        }

        bool parseStackLimitValue(const std::string& input, StackSize& out, std::string& error)
        {
            std::string trimmed = trimCopy(input);
            if (trimmed.empty())
            {
                error = "stack limit is empty";
                return false;
            }

            std::size_t digitCount = 0;
            while (digitCount < trimmed.size() &&
                   std::isdigit(static_cast<unsigned char>(trimmed[digitCount])))
            {
                ++digitCount;
            }
            if (digitCount == 0)
            {
                error = "stack limit must start with a number";
                return false;
            }

            const std::string numberPart = trimmed.substr(0, digitCount);
            std::string suffix = trimCopy(trimmed.substr(digitCount));

            unsigned long long base = 0;
            auto [ptr, ec] =
                std::from_chars(numberPart.data(), numberPart.data() + numberPart.size(), base, 10);
            if (ec != std::errc() || ptr != numberPart.data() + numberPart.size())
            {
                error = "invalid numeric value";
                return false;
            }
            if (base == 0)
            {
                error = "stack limit must be greater than zero";
                return false;
            }

            StackSize multiplier = 1;
            if (!suffix.empty())
            {
                std::string lowered;
                lowered.reserve(suffix.size());
                for (char c : suffix)
                {
                    lowered.push_back(
                        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                }

                if (lowered == "b")
                {
                    multiplier = 1;
                }
                else if (lowered == "k" || lowered == "kb" || lowered == "kib")
                {
                    multiplier = 1024ull;
                }
                else if (lowered == "m" || lowered == "mb" || lowered == "mib")
                {
                    multiplier = 1024ull * 1024ull;
                }
                else if (lowered == "g" || lowered == "gb" || lowered == "gib")
                {
                    multiplier = 1024ull * 1024ull * 1024ull;
                }
                else
                {
                    error = "unsupported suffix (use bytes, KiB, MiB, or GiB)";
                    return false;
                }
            }

            if (base > std::numeric_limits<StackSize>::max() / multiplier)
            {
                error = "stack limit is too large";
                return false;
            }

            out = static_cast<StackSize>(base) * multiplier;
            return true;
        }

        bool parseBoolSwitch(const std::string& input, bool& out, std::string& error)
        {
            std::string lowered;
            lowered.reserve(input.size());
            for (char c : trimCopy(input))
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

            if (lowered == "true" || lowered == "on" || lowered == "yes" || lowered == "1")
            {
                out = true;
                return true;
            }
            if (lowered == "false" || lowered == "off" || lowered == "no" || lowered == "0")
            {
                out = false;
                return true;
            }

            error = "expected true/false (or on/off, yes/no, 1/0)";
            return false;
        }

        std::string stripOptionalQuotes(const std::string& input)
        {
            if (input.size() >= 2)
            {
                const char first = input.front();
                const char last = input.back();
                if ((first == '"' && last == '"') || (first == '\'' && last == '\''))
                    return input.substr(1, input.size() - 2);
            }
            return input;
        }

        std::string toLowerAsciiCopy(const std::string& input)
        {
            std::string lowered;
            lowered.reserve(input.size());
            for (char c : input)
                lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return lowered;
        }

        std::string resolveConfigRelativePath(const std::string& rawPath,
                                              const std::filesystem::path& configDir)
        {
            if (rawPath.empty())
                return rawPath;
            std::filesystem::path p(rawPath);
            if (p.is_absolute())
                return p.lexically_normal().string();
            return (configDir / p).lexically_normal().string();
        }

        void addCsvFilters(std::vector<std::string>& dest, const std::string& input)
        {
            std::string current;
            for (char c : input)
            {
                if (c == ',')
                {
                    std::string trimmed = trimCopy(current);
                    if (!trimmed.empty())
                        dest.push_back(trimmed);
                    current.clear();
                }
                else
                {
                    current.push_back(c);
                }
            }
            std::string trimmed = trimCopy(current);
            if (!trimmed.empty())
                dest.push_back(trimmed);
        }

        bool consumeLongOptionValue(const std::string& argStr, const char* optionName, int& i,
                                    int argc, char** argv, std::string& valueOut,
                                    std::string& errorOut)
        {
            if (argStr == optionName)
            {
                if (i + 1 >= argc)
                {
                    errorOut = "Missing argument for " + std::string(optionName);
                    return true;
                }
                valueOut = argv[++i];
                return true;
            }
            const std::string prefix = std::string(optionName) + "=";
            if (argStr.rfind(prefix, 0) == 0)
            {
                valueOut = argStr.substr(prefix.size());
                return true;
            }
            return false;
        }

        enum class SmtOptionSource
        {
            Config,
            Cli
        };

        using SmtOptionApplyFn =
            bool (*)(AnalysisConfig& cfg, const std::string& value, SmtOptionSource source,
                     std::string& error);

        struct SmtOptionSpec
        {
            std::string_view configKey;
            const char* cliOption;
            SmtOptionApplyFn apply = nullptr;
            std::uint64_t impliesSmtEnabled : 1 = true;
            std::uint64_t reservedFlags : 63 = 0;
        };

        bool applySmtSwitchOption(AnalysisConfig& cfg, const std::string& value,
                                  SmtOptionSource source, std::string& error)
        {
            bool parsedValue = false;
            if (source == SmtOptionSource::Cli)
            {
                if (!parseSmtSwitch(value, parsedValue, error))
                    return false;
            }
            else
            {
                if (!parseBoolSwitch(value, parsedValue, error))
                    return false;
            }
            cfg.smtEnabled = parsedValue;
            return true;
        }

        bool applySmtBackendOption(AnalysisConfig& cfg, const std::string& value, SmtOptionSource,
                                   std::string&)
        {
            cfg.smtBackend = value;
            return true;
        }

        bool applySmtSecondaryBackendOption(AnalysisConfig& cfg, const std::string& value,
                                            SmtOptionSource, std::string&)
        {
            cfg.smtSecondaryBackend = value;
            return true;
        }

        bool applySmtModeOption(AnalysisConfig& cfg, const std::string& value, SmtOptionSource,
                                std::string& error)
        {
            analysis::smt::SolverMode parsedMode = cfg.smtMode;
            if (!parseSmtMode(value, parsedMode, error))
                return false;
            cfg.smtMode = parsedMode;
            return true;
        }

        bool applySmtTimeoutOption(AnalysisConfig& cfg, const std::string& value, SmtOptionSource,
                                   std::string& error)
        {
            unsigned parsedTimeout = 0;
            if (!parsePositiveUnsigned(value, parsedTimeout, error))
                return false;
            cfg.smtTimeoutMs = parsedTimeout;
            return true;
        }

        bool applySmtBudgetOption(AnalysisConfig& cfg, const std::string& value, SmtOptionSource,
                                  std::string& error)
        {
            std::uint64_t parsedBudget = 0;
            if (!parsePositiveU64(value, parsedBudget, error))
                return false;
            cfg.smtBudgetNodes = parsedBudget;
            return true;
        }

        bool applySmtRulesOption(AnalysisConfig& cfg, const std::string& value,
                                 SmtOptionSource source, std::string&)
        {
            if (source == SmtOptionSource::Config)
                cfg.smtRules.clear();
            addCsvFilters(cfg.smtRules, value);
            return true;
        }

        constexpr std::array<SmtOptionSpec, 7> kSmtOptionSpecs = {{
            {"smt", "--smt", &applySmtSwitchOption, false},
            {"smt-backend", "--smt-backend", &applySmtBackendOption, true},
            {"smt-secondary-backend", "--smt-secondary-backend", &applySmtSecondaryBackendOption,
             true},
            {"smt-mode", "--smt-mode", &applySmtModeOption, true},
            {"smt-timeout-ms", "--smt-timeout-ms", &applySmtTimeoutOption, true},
            {"smt-budget-nodes", "--smt-budget-nodes", &applySmtBudgetOption, true},
            {"smt-rules", "--smt-rules", &applySmtRulesOption, true},
        }};

        const SmtOptionSpec* findSmtOptionByConfigKey(std::string_view key)
        {
            for (const SmtOptionSpec& spec : kSmtOptionSpecs)
            {
                if (spec.configKey == key)
                    return &spec;
            }
            return nullptr;
        }

        bool applySmtOptionValue(const SmtOptionSpec& spec, AnalysisConfig& cfg,
                                 const std::string& value, SmtOptionSource source,
                                 std::string& error)
        {
            if (!spec.apply || !spec.apply(cfg, value, source, error))
                return false;
            if (spec.impliesSmtEnabled)
                cfg.smtEnabled = true;
            return true;
        }

        bool tryConsumeAndApplySmtCliOption(const std::string& argStr, int& i, int argc,
                                            char** argv, AnalysisConfig& cfg, std::string& error)
        {
            for (const SmtOptionSpec& spec : kSmtOptionSpecs)
            {
                std::string value;
                std::string consumeError;
                if (!consumeLongOptionValue(argStr, spec.cliOption, i, argc, argv, value,
                                            consumeError))
                {
                    continue;
                }

                if (!consumeError.empty())
                {
                    error = consumeError;
                    return true;
                }

                std::string valueError;
                if (!applySmtOptionValue(spec, cfg, value, SmtOptionSource::Cli, valueError))
                {
                    error = "Invalid " + std::string(spec.cliOption) + " value: " + valueError;
                    return true;
                }

                return true;
            }
            return false;
        }

        template <typename Owner> struct BoolConfigSpec
        {
            std::string_view key;
            void (*set)(Owner&, bool) = nullptr;
        };

        void setConfigTiming(AnalysisConfig& cfg, bool value)
        {
            cfg.timing = value;
        }

        void setConfigWarningsOnly(AnalysisConfig& cfg, bool value)
        {
            cfg.warningsOnly = value;
        }

        void setConfigQuiet(AnalysisConfig& cfg, bool value)
        {
            cfg.quiet = value;
        }

        void setConfigDemangle(AnalysisConfig& cfg, bool value)
        {
            cfg.demangle = value;
        }

        void setConfigResourceCrossTU(AnalysisConfig& cfg, bool value)
        {
            cfg.resourceCrossTU = value;
        }

        void setConfigUninitializedCrossTU(AnalysisConfig& cfg, bool value)
        {
            cfg.uninitializedCrossTU = value;
        }

        void setConfigResourceSummaryMemoryOnly(AnalysisConfig& cfg, bool value)
        {
            cfg.resourceSummaryMemoryOnly = value;
        }

        void setParsedIncludeCompdbDeps(ParsedArguments& parsed, bool value)
        {
            parsed.includeCompdbDeps = value;
        }

        constexpr std::array<BoolConfigSpec<AnalysisConfig>, 7> kConfigBoolSpecs = {{
            {"timing", &setConfigTiming},
            {"warnings-only", &setConfigWarningsOnly},
            {"quiet", &setConfigQuiet},
            {"demangle", &setConfigDemangle},
            {"resource-cross-tu", &setConfigResourceCrossTU},
            {"uninitialized-cross-tu", &setConfigUninitializedCrossTU},
            {"resource-summary-cache-memory-only", &setConfigResourceSummaryMemoryOnly},
        }};

        constexpr std::array<BoolConfigSpec<ParsedArguments>, 1> kParsedBoolSpecs = {{
            {"include-compdb-deps", &setParsedIncludeCompdbDeps},
        }};

        template <typename Owner, std::size_t N>
        bool tryApplyBoolConfigSpec(std::string_view key, const std::string& value, Owner& owner,
                                    const std::array<BoolConfigSpec<Owner>, N>& specs,
                                    std::string& error)
        {
            for (const BoolConfigSpec<Owner>& spec : specs)
            {
                if (spec.key != key)
                    continue;

                bool parsedValue = false;
                std::string localError;
                if (!parseBoolSwitch(value, parsedValue, localError))
                {
                    error = "invalid " + std::string(spec.key) + " value: " + localError;
                    return false;
                }

                if (spec.set)
                    spec.set(owner, parsedValue);
                return true;
            }
            return false;
        }

        struct PreParsedCliMeta
        {
            std::string configPath;
            std::uint64_t printEffectiveConfig : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;
        };

        bool preScanMetaOptions(int argc, char** argv, PreParsedCliMeta& outMeta,
                                std::string& error)
        {
            outMeta = {};
            for (int i = 1; i < argc; ++i)
            {
                const std::string argStr{argv[i]};
                if (argStr == "--print-effective-config")
                {
                    outMeta.printEffectiveConfig = true;
                    continue;
                }

                std::string value;
                if (consumeLongOptionValue(argStr, "--config", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return false;
                    outMeta.configPath = value;
                    continue;
                }
            }
            return true;
        }

        bool applyConfigEntry(const std::string& keyRaw, const std::string& valueRaw, ParsedArguments& parsed,
                              AnalysisConfig& cfg, const std::filesystem::path& configDir,
                              std::string& error)
        {
            std::string key = toLowerAsciiCopy(trimCopy(keyRaw));
            for (char& c : key)
            {
                if (c == '_')
                    c = '-';
            }

            const std::string value = stripOptionalQuotes(trimCopy(valueRaw));
            if (key.empty())
            {
                error = "empty key";
                return false;
            }

            if (key == "resource-model")
            {
                cfg.resourceModelPath = resolveConfigRelativePath(value, configDir);
                return true;
            }
            if (key == "escape-model")
            {
                cfg.escapeModelPath = resolveConfigRelativePath(value, configDir);
                return true;
            }
            if (key == "buffer-model")
            {
                cfg.bufferModelPath = resolveConfigRelativePath(value, configDir);
                return true;
            }
            if (key == "compile-commands" || key == "compdb")
            {
                parsed.compileCommandsPath = resolveConfigRelativePath(value, configDir);
                parsed.compileCommandsExplicit = true;
                return true;
            }
            if (key == "analysis-profile")
            {
                std::string localError;
                if (!parseAnalysisProfile(value, cfg.profile, localError))
                {
                    error = "invalid analysis profile: " + localError;
                    return false;
                }
                parsed.analysisProfileExplicit = true;
                return true;
            }
            if (key == "jobs")
            {
                unsigned parsedJobs = 0;
                bool parsedAuto = false;
                std::string localError;
                if (!parseJobsValue(value, parsedJobs, parsedAuto, localError))
                {
                    error = "invalid jobs value: " + localError;
                    return false;
                }
                cfg.jobs = parsedJobs;
                cfg.jobsAuto = parsedAuto;
                return true;
            }
            {
                std::string boolError;
                if (tryApplyBoolConfigSpec(key, value, cfg, kConfigBoolSpecs, boolError))
                    return true;
                if (!boolError.empty())
                {
                    error = std::move(boolError);
                    return false;
                }
            }
            {
                std::string boolError;
                if (tryApplyBoolConfigSpec(key, value, parsed, kParsedBoolSpecs, boolError))
                    return true;
                if (!boolError.empty())
                {
                    error = std::move(boolError);
                    return false;
                }
            }
            if (const SmtOptionSpec* smtSpec = findSmtOptionByConfigKey(key))
            {
                std::string localError;
                if (!applySmtOptionValue(*smtSpec, cfg, value, SmtOptionSource::Config, localError))
                {
                    error = "invalid " + key + " value: " + localError;
                    return false;
                }
                return true;
            }
            if (key == "resource-summary-cache-dir")
            {
                cfg.resourceSummaryCacheDir = resolveConfigRelativePath(value, configDir);
                return true;
            }
            if (key == "compile-ir-cache-dir")
            {
                cfg.compileIRCacheDir = resolveConfigRelativePath(value, configDir);
                return true;
            }

            error = "unknown key '" + key + "'";
            return false;
        }

        bool loadConfigFile(const std::string& configPathRaw, ParsedArguments& parsed,
                            AnalysisConfig& cfg, std::string& error)
        {
            const std::filesystem::path configPath = std::filesystem::path(configPathRaw);
            std::ifstream in(configPath);
            if (!in)
            {
                error = "Unable to open config file: " + configPathRaw;
                return false;
            }

            std::filesystem::path baseDir = configPath.parent_path();
            if (baseDir.empty())
                baseDir = std::filesystem::current_path();

            std::string line;
            std::size_t lineNo = 0;
            while (std::getline(in, line))
            {
                ++lineNo;
                const std::string trimmed = trimCopy(line);
                if (trimmed.empty() || trimmed.rfind("#", 0) == 0 || trimmed.rfind(";", 0) == 0 ||
                    trimmed.rfind("//", 0) == 0)
                {
                    continue;
                }

                const std::size_t eqPos = trimmed.find('=');
                if (eqPos == std::string::npos)
                {
                    std::ostringstream oss;
                    oss << "Invalid config syntax at line " << lineNo << ": expected key=value";
                    error = oss.str();
                    return false;
                }

                const std::string key = trimCopy(trimmed.substr(0, eqPos));
                const std::string value = trimCopy(trimmed.substr(eqPos + 1));
                if (key.empty())
                {
                    std::ostringstream oss;
                    oss << "Invalid config syntax at line " << lineNo << ": empty key";
                    error = oss.str();
                    return false;
                }

                std::string applyError;
                if (!applyConfigEntry(key, value, parsed, cfg, baseDir, applyError))
                {
                    std::ostringstream oss;
                    oss << "Invalid config entry at line " << lineNo << ": " << applyError;
                    error = oss.str();
                    return false;
                }
            }

            return true;
        }

        ParseResult makeError(std::string error)
        {
            ParseResult result;
            result.status = ParseStatus::Error;
            result.error = std::move(error);
            return result;
        }

        std::string unknownOptionErrorWithSuggestion(const std::string& argStr)
        {
            std::string error = "Unknown option: " + argStr;
            if (auto suggestion = UnknownOptionSuggester::suggest(argStr))
            {
                error += ". Did you mean '" + *suggestion + "'?";
            }
            return error;
        }

        bool splitCommandLine(const std::string& commandLine, std::vector<std::string>& outArgs,
                              std::string& error)
        {
            outArgs.clear();
            std::string current;

            bool inSingleQuote = false;
            bool inDoubleQuote = false;
            bool escaping = false;

            auto flushCurrent = [&]()
            {
                if (!current.empty())
                {
                    outArgs.push_back(current);
                    current.clear();
                }
            };

            for (std::size_t i = 0; i < commandLine.size(); ++i)
            {
                const char ch = commandLine[i];

                if (escaping)
                {
                    current.push_back(ch);
                    escaping = false;
                    continue;
                }

                if (inSingleQuote)
                {
                    if (ch == '\'')
                    {
                        inSingleQuote = false;
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                    continue;
                }

                if (inDoubleQuote)
                {
                    if (ch == '"')
                    {
                        inDoubleQuote = false;
                    }
                    else if (ch == '\\')
                    {
                        escaping = true;
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                    continue;
                }

                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    flushCurrent();
                    continue;
                }

                if (ch == '\'')
                {
                    inSingleQuote = true;
                    continue;
                }

                if (ch == '"')
                {
                    inDoubleQuote = true;
                    continue;
                }

                if (ch == '\\')
                {
                    escaping = true;
                    continue;
                }

                current.push_back(ch);
            }

            if (escaping)
            {
                error = "Invalid command line: dangling escape at end of input";
                return false;
            }
            if (inSingleQuote || inDoubleQuote)
            {
                error = "Invalid command line: unterminated quoted string";
                return false;
            }

            flushCurrent();
            return true;
        }
    } // namespace

    ParseResult parseArguments(int argc, char** argv)
    {
        ParseResult result;
        ParsedArguments& parsed = result.parsed;
        AnalysisConfig& cfg = parsed.config;
        cfg.quiet = false;
        cfg.warningsOnly = false;
        cfg.extraCompileArgs.emplace_back("-O0");
        cfg.extraCompileArgs.emplace_back("--ct-optnone");

        PreParsedCliMeta meta;
        std::string preScanError;
        if (!preScanMetaOptions(argc, argv, meta, preScanError))
            return makeError(preScanError);
        parsed.printEffectiveConfig = meta.printEffectiveConfig;
        if (!meta.configPath.empty())
        {
            std::string configError;
            if (!loadConfigFile(meta.configPath, parsed, cfg, configError))
                return makeError(configError);
            parsed.configPath = meta.configPath;
        }

        for (int i = 1; i < argc; ++i)
        {
            const char* arg = argv[i];
            std::string argStr{arg};

            if (argStr == "-h" || argStr == "--help")
            {
                result.status = ParseStatus::Help;
                return result;
            }
            if (argStr == "--demangle")
            {
                cfg.demangle = true;
                continue;
            }
            if (argStr == "--quiet")
            {
                cfg.quiet = true;
                continue;
            }
            if (argStr == "--verbose")
            {
                cfg.quiet = false;
                parsed.verbose = true;
                continue;
            }
            if (argStr == "--print-effective-config")
            {
                parsed.printEffectiveConfig = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--config", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    parsed.configPath = value;
                    continue;
                }
            }
            if (argStr == "--STL" || argStr == "--stl")
            {
                cfg.includeSTL = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--only-file", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.onlyFiles.emplace_back(std::move(value));
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--only-func", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    addCsvFilters(cfg.onlyFunctions, value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--only-function", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    addCsvFilters(cfg.onlyFunctions, value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--only-dir", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.onlyDirs.emplace_back(std::move(value));
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--exclude-dir", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    addCsvFilters(cfg.excludeDirs, value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--stack-limit", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    StackSize parsedStackLimit = 0;
                    if (!parseStackLimitValue(value, parsedStackLimit, error))
                        return makeError("Invalid --stack-limit value: " + error);
                    cfg.stackLimit = parsedStackLimit;
                    continue;
                }
            }
            if (argStr == "--dump-filter")
            {
                cfg.dumpFilter = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--dump-ir", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.dumpIRPath = std::move(value);
                    continue;
                }
            }
            if (argStr == "-I")
            {
                if (i + 1 >= argc)
                    return makeError("Missing argument for -I");
                cfg.extraCompileArgs.emplace_back("-I" + std::string(argv[++i]));
                continue;
            }
            if (argStr.rfind("-I", 0) == 0 && argStr.size() > 2)
            {
                cfg.extraCompileArgs.emplace_back(argStr);
                continue;
            }
            if (argStr == "-D")
            {
                if (i + 1 >= argc)
                    return makeError("Missing argument for -D");
                cfg.extraCompileArgs.emplace_back("-D" + std::string(argv[++i]));
                continue;
            }
            if (argStr.rfind("-D", 0) == 0 && argStr.size() > 2)
            {
                cfg.extraCompileArgs.emplace_back(argStr);
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--compile-arg", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.extraCompileArgs.emplace_back(std::move(value));
                    continue;
                }
            }
            if (argStr == "--compdb-fast")
            {
                cfg.compdbFast = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--analysis-profile", i, argc, argv, value,
                                           error))
                {
                    if (!error.empty())
                        return makeError(error);
                    if (!parseAnalysisProfile(value, cfg.profile, error))
                        return makeError("Invalid --analysis-profile value: " + error);
                    parsed.analysisProfileExplicit = true;
                    continue;
                }
            }
            if (argStr == "--include-compdb-deps")
            {
                parsed.includeCompdbDeps = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--jobs", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    unsigned parsedJobs = 0;
                    bool parsedAuto = false;
                    if (!parseJobsValue(value, parsedJobs, parsedAuto, error))
                        return makeError("Invalid --jobs value: " + error);
                    cfg.jobs = parsedJobs;
                    cfg.jobsAuto = parsedAuto;
                    continue;
                }
            }
            if (argStr == "--timing")
            {
                cfg.timing = true;
                continue;
            }
            {
                std::string error;
                if (tryConsumeAndApplySmtCliOption(argStr, i, argc, argv, cfg, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--resource-model", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.resourceModelPath = std::move(value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--escape-model", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.escapeModelPath = std::move(value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--buffer-model", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.bufferModelPath = std::move(value);
                    continue;
                }
            }
            if (argStr == "--resource-cross-tu")
            {
                cfg.resourceCrossTU = true;
                continue;
            }
            if (argStr == "--no-resource-cross-tu")
            {
                cfg.resourceCrossTU = false;
                continue;
            }
            if (argStr == "--uninitialized-cross-tu")
            {
                cfg.uninitializedCrossTU = true;
                continue;
            }
            if (argStr == "--no-uninitialized-cross-tu")
            {
                cfg.uninitializedCrossTU = false;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--resource-summary-cache-dir", i, argc, argv,
                                           value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.resourceSummaryCacheDir = std::move(value);
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--compile-ir-cache-dir", i, argc, argv, value,
                                           error))
                {
                    if (!error.empty())
                        return makeError(error);
                    cfg.compileIRCacheDir = std::move(value);
                    continue;
                }
            }
            if (argStr == "--resource-summary-cache-memory-only")
            {
                cfg.resourceSummaryMemoryOnly = true;
                continue;
            }
            if (argStr == "--compile-commands" || argStr == "--compdb")
            {
                if (i + 1 >= argc)
                    return makeError("Missing argument for " + argStr);
                parsed.compileCommandsPath = argv[++i];
                parsed.compileCommandsExplicit = true;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--compile-commands", i, argc, argv, value,
                                           error))
                {
                    if (!error.empty())
                        return makeError(error);
                    parsed.compileCommandsPath = std::move(value);
                    parsed.compileCommandsExplicit = true;
                    continue;
                }
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--compdb", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    parsed.compileCommandsPath = std::move(value);
                    parsed.compileCommandsExplicit = true;
                    continue;
                }
            }
            if (argStr == "--warnings-only")
            {
                cfg.warningsOnly = true;
                continue;
            }
            if (argStr == "--format=json")
            {
                parsed.outputFormat = OutputFormat::Json;
                continue;
            }
            if (argStr == "--format=sarif")
            {
                parsed.outputFormat = OutputFormat::Sarif;
                continue;
            }
            if (argStr == "--format=human")
            {
                parsed.outputFormat = OutputFormat::Human;
                continue;
            }
            {
                std::string value;
                std::string error;
                if (consumeLongOptionValue(argStr, "--base-dir", i, argc, argv, value, error))
                {
                    if (!error.empty())
                        return makeError(error);
                    parsed.sarifBaseDir = std::move(value);
                    continue;
                }
            }
            if (std::strncmp(arg, "--mode=", 7) == 0)
            {
                const char* modeStr = arg + 7;
                if (std::strcmp(modeStr, "ir") == 0)
                {
                    cfg.mode = AnalysisMode::IR;
                }
                else if (std::strcmp(modeStr, "abi") == 0)
                {
                    cfg.mode = AnalysisMode::ABI;
                }
                else
                {
                    return makeError("Unknown mode: " + std::string(modeStr) +
                                     " (expected 'ir' or 'abi')");
                }
                continue;
            }
            if (!argStr.empty() && argStr[0] == '-')
                return makeError(unknownOptionErrorWithSuggestion(argStr));

            parsed.inputFilenames.emplace_back(std::move(argStr));
        }

        return result;
    }

    ParseResult parseArguments(const std::vector<std::string>& analyzerArgs)
    {
        std::vector<std::string> argvStorage;
        argvStorage.reserve(analyzerArgs.size() + 1);
        argvStorage.emplace_back("stack_usage_analyzer");
        argvStorage.insert(argvStorage.end(), analyzerArgs.begin(), analyzerArgs.end());

        std::vector<char*> argvPointers;
        argvPointers.reserve(argvStorage.size());
        for (auto& arg : argvStorage)
            argvPointers.push_back(const_cast<char*>(arg.c_str()));

        return parseArguments(static_cast<int>(argvPointers.size()), argvPointers.data());
    }

    ParseResult parseCommandLine(const std::string& commandLine)
    {
        std::vector<std::string> args;
        std::string splitError;
        if (!splitCommandLine(commandLine, args, splitError))
            return makeError(splitError);
        return parseArguments(args);
    }

} // namespace ctrace::stack::cli
