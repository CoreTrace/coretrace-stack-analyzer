// SPDX-License-Identifier: Apache-2.0
#include "StackUsageAnalyzer.hpp"
#include "RuleCatalog.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio> // std::snprintf
#include <filesystem>
#include <iomanip>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ctrace::stack
{
    namespace
    {

        // Small helper to escape JSON strings.
        static std::string jsonEscape(const std::string& s)
        {
            std::string out;
            out.reserve(s.size() + 16);
            for (char c : s)
            {
                switch (c)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '\"':
                    out += "\\\"";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        char buf[7] = {};
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c & 0xFF);
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
                }
            }
            return out;
        }

        // Old helper to convert DiagnosticSeverity to string, don't use it anymore.
        static const char* severityToJsonString(DiagnosticSeverity sev)
        {
            switch (sev)
            {
            case DiagnosticSeverity::Info:
                return "info";
            case DiagnosticSeverity::Warning:
                return "warning";
            case DiagnosticSeverity::Error:
                return "error";
            }
            return "info";
        }

        static const char* severityToSarifLevel(DiagnosticSeverity sev)
        {
            // SARIF levels: "none", "note", "warning", "error"
            switch (sev)
            {
            case DiagnosticSeverity::Info:
                return "note";
            case DiagnosticSeverity::Warning:
                return "warning";
            case DiagnosticSeverity::Error:
                return "error";
            }
            return "note";
        }

        /// Number of a "CWE-<n>" id, or std::nullopt for anything else.
        static std::optional<unsigned> parseCweNumber(std::string_view id)
        {
            constexpr std::string_view prefix = "CWE-";
            if (!id.starts_with(prefix))
                return std::nullopt;
            id.remove_prefix(prefix.size());
            unsigned number = 0;
            const auto [end, error] = std::from_chars(id.data(), id.data() + id.size(), number);
            if (error != std::errc{} || end != id.data() + id.size())
                return std::nullopt;
            return number;
        }

        /// CWE tag in CodeQL's form, which GitHub reads: external/cwe/cwe-078.
        static std::string cweTag(unsigned number)
        {
            std::string digits = std::to_string(number);
            if (digits.size() < 3)
                digits.insert(0, 3 - digits.size(), '0');
            return "external/cwe/cwe-" + digits;
        }

        /// GitHub code-scanning security severity of CWE @p cwe in tenths (93 is 9.3), or 0 (no
        /// severity, for GitHub too) when the CWE is not a security weakness. GitHub scores a
        /// CodeQL query as the 75th percentile of the CVSS scores of the CVEs sharing its CWE
        /// tags; each CWE takes, from the first rule that gives one:
        ///  1. the score of the CodeQL C/C++ query that detects the same weakness;
        ///  2. the score of CodeQL's queries tagged with this CWE alone, in any language, or no
        ///     score when they are quality queries;
        ///  3. the score of the closest CWE with a C/C++ query in the CWE research view: parent
        ///     or child, then sibling; the highest when several are as close.
        /// The comment beside each case names the query, or the rule and CWE it comes through.
        static unsigned securitySeverityTenths(unsigned cwe)
        {
            switch (cwe)
            {
            case 676: // cpp/dangerous-function-overflow (gets)
                return 100;
            case 78: // cpp/command-line-injection
                return 98;
            case 120: // cpp/unbounded-write
            case 121: // cpp/overflow-buffer
            case 124: // rule 3, parent 787 (out-of-bounds write): cpp/unbounded-write
            case 125: // cpp/invalid-pointer-deref
            case 127: // rule 3, parent 125 (out-of-bounds read): cpp/invalid-pointer-deref
            case 134: // cpp/non-constant-format
            case 415: // cpp/double-free
            case 416: // cpp/use-after-free
            case 562: // cpp/return-stack-allocated-memory
            case 787: // cpp/unbounded-write
            case 823: // cpp/missing-negativity-test
            case 843: // cpp/type-confusion
                return 93;
            case 467: // cpp/suspicious-sizeof
                return 88;
            case 191: // cpp/uncontrolled-arithmetic
                return 86;
            case 190: // cpp/integer-overflow-tainted
            case 195: // rule 3, parent 681: cpp/integer-overflow-tainted
            case 197: // cpp/integer-overflow-tainted
            case 789: // cpp/uncontrolled-allocation-size
                return 81;
            case 457: // cpp/uninitialized-local
            case 665: // cpp/uninitialized-local
            case 772: // rule 3, the higher child, 775: cpp/descriptor-never-closed
                return 78;
            case 367: // cpp/toctou-race-condition
                return 77;
            case 476: // cpp/missing-null-test
            case 770: // cpp/alloca-in-loop
            case 674: // rule 3, sibling 835 (infinite loop) under 834:
                      // cpp/infinite-loop-with-unsatisfiable-exit-condition
                return 75;
            case 200: // rule 2: every CodeQL query tagged CWE-200 alone scores 6.5
                return 65;
            case 685: // cpp/wrong-number-format-arguments
                return 50;
            }
            // Dead code (561) among them, by rule 2: CodeQL's duplicate-condition queries, which
            // carry it alone, are quality queries, not security ones.
            return 0;
        }

        static std::string resolveRuleId(const Diagnostic& d)
        {
            if (!d.ruleId.empty())
                return d.ruleId;
            return std::string(ctrace::stack::enumToString(d.errCode));
        }

        static std::string formatConfidence(double confidence)
        {
            std::ostringstream os;
            os << std::fixed << std::setprecision(2) << confidence;
            return os.str();
        }

        // Strip a base directory prefix from a file path to produce a relative URI.
        // If baseDir is empty, the path is returned unchanged (backward-compatible).
        static std::string stripBase(const std::string& path, const std::string& baseDir)
        {
            if (baseDir.empty())
                return path;

            std::filesystem::path canonical;
            try
            {
                canonical = std::filesystem::canonical(baseDir);
            }
            catch (...)
            {
                canonical = std::filesystem::path(baseDir);
            }
            std::string base = canonical.string();

            // Ensure the base ends with a separator.
            if (!base.empty() && base.back() != '/')
                base += '/';

            if (path.size() >= base.size() && path.compare(0, base.size(), base) == 0)
                return path.substr(base.size());

            return path;
        }

    } // anonymous namespace

    static std::string toJsonImpl(const AnalysisResult& result, const std::string* inputFile,
                                  const std::vector<std::string>* inputFiles)
    {
        const DiagnosticSummary diagnosticsSummary = summarizeDiagnostics(result);
        std::ostringstream os;
        os << "{\n";
        os << "  \"meta\": {\n";
        os << "    \"tool\": \""
           << "ctrace-stack-analyzer"
           << "\",\n";
        if (inputFiles && !inputFiles->empty())
        {
            os << "    \"inputFiles\": [";
            for (std::size_t i = 0; i < inputFiles->size(); ++i)
            {
                os << "\"" << jsonEscape((*inputFiles)[i]) << "\"";
                if (i + 1 < inputFiles->size())
                    os << ", ";
            }
            os << "],\n";
        }
        else if (inputFile)
        {
            os << "    \"inputFile\": \"" << jsonEscape(*inputFile) << "\",\n";
        }
        os << "    \"mode\": \"" << (result.config.mode == AnalysisMode::IR ? "IR" : "ABI")
           << "\",\n";
        os << "    \"stackLimit\": " << result.config.stackLimit << ",\n";
        os << "    \"analysisTimeMs\": " << -1 << "\n";
        os << " },\n";

        // Functions
        os << "  \"functions\": [\n";
        for (std::size_t i = 0; i < result.functions.size(); ++i)
        {
            const auto& f = result.functions[i];
            os << "    {\n";
            std::string filePath = f.filePath;
            if (filePath.empty() && inputFile)
            {
                filePath = *inputFile;
            }
            os << "      \"file\": \"" << jsonEscape(filePath) << "\",\n";
            os << "      \"name\": \"" << jsonEscape(f.name) << "\",\n";
            os << "      \"localStack\": ";
            if (f.localStackUnknown)
            {
                os << "null";
            }
            else
            {
                os << f.localStack;
            }
            os << ",\n";
            os << "      \"localStackLowerBound\": ";
            if (f.localStackUnknown && f.localStack > 0)
            {
                os << f.localStack;
            }
            else
            {
                os << "null";
            }
            os << ",\n";
            os << "      \"localStackUnknown\": " << (f.localStackUnknown ? "true" : "false")
               << ",\n";
            os << "      \"maxStack\": ";
            if (f.maxStackUnknown)
            {
                os << "null";
            }
            else
            {
                os << f.maxStack;
            }
            os << ",\n";
            os << "      \"maxStackLowerBound\": ";
            if (f.maxStackUnknown && f.maxStack > 0)
            {
                os << f.maxStack;
            }
            else
            {
                os << "null";
            }
            os << ",\n";
            os << "      \"maxStackUnknown\": " << (f.maxStackUnknown ? "true" : "false") << ",\n";
            os << "      \"hasDynamicAlloca\": " << (f.hasDynamicAlloca ? "true" : "false")
               << ",\n";
            os << "      \"isRecursive\": " << (f.isRecursive ? "true" : "false") << ",\n";
            os << "      \"hasInfiniteSelfRecursion\": "
               << (f.hasInfiniteSelfRecursion ? "true" : "false") << ",\n";
            os << "      \"exceedsLimit\": " << (f.exceedsLimit ? "true" : "false") << "\n";
            os << "    }";
            if (i + 1 < result.functions.size())
                os << ",";
            os << "\n";
        }
        os << "  ],\n";

        // Diagnostics
        os << "  \"diagnostics\": [\n";
        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            const auto& d = result.diagnostics[i];
            os << "    {\n";
            os << "      \"id\": \"diag-" << (i + 1) << "\",\n";
            os << "      \"severity\": \"" << ctrace::stack::enumToString(d.severity) << "\",\n";
            const std::string ruleId = resolveRuleId(d);
            os << "      \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";
            os << "      \"confidence\": ";
            if (d.confidence >= 0.0)
                os << formatConfidence(d.confidence);
            else
                os << "null";
            os << ",\n";
            os << "      \"cwe\": ";
            if (!d.cweId.empty())
                os << "\"" << jsonEscape(d.cweId) << "\"";
            else
                os << "null";
            os << ",\n";

            std::string diagFilePath = d.filePath;
            if (diagFilePath.empty() && inputFile)
            {
                diagFilePath = *inputFile;
            }
            os << "      \"location\": {\n";
            os << "        \"file\": \"" << jsonEscape(diagFilePath) << "\",\n";
            os << "        \"function\": \"" << jsonEscape(d.funcName) << "\",\n";
            os << "        \"startLine\": " << d.line << ",\n";
            os << "        \"startColumn\": " << d.column << ",\n";
            os << "        \"endLine\": " << d.endLine << ",\n";
            os << "        \"endColumn\": " << d.endColumn << "\n";
            os << "      },\n";

            os << "      \"details\": {\n";
            os << "        \"message\": \"" << jsonEscape(d.message) << "\",\n";
            os << "        \"variableAliasing\": [";
            for (std::size_t j = 0; j < d.variableAliasingVec.size(); ++j)
            {
                os << "\"" << jsonEscape(d.variableAliasingVec[j]) << "\"";
                if (j + 1 < d.variableAliasingVec.size())
                    os << ", ";
            }
            os << "]\n";
            os << "      }\n"; // <-- ferme "details"
            os << "    }";     // <-- ferme le diagnostic
            if (i + 1 < result.diagnostics.size())
                os << ",";
            os << "\n";
        }
        os << "  ],\n";
        os << "  \"diagnosticsSummary\": {\n";
        os << "    \"info\": " << diagnosticsSummary.info << ",\n";
        os << "    \"warning\": " << diagnosticsSummary.warning << ",\n";
        os << "    \"error\": " << diagnosticsSummary.error << "\n";
        os << "  }\n";
        os << "}\n";
        return os.str();
    }

    std::string toJson(const AnalysisResult& result, const std::string& inputFile)
    {
        return toJsonImpl(result, &inputFile, nullptr);
    }

    std::string toJson(const AnalysisResult& result, const std::vector<std::string>& inputFiles)
    {
        return toJsonImpl(result, nullptr, &inputFiles);
    }

    std::string toSarif(const AnalysisResult& result, const std::string& inputFile,
                        const std::string& toolName, const std::string& toolVersion,
                        const std::string& baseDir)
    {
        struct SarifRuleEntry
        {
            std::string id;
            // Every CWE the rule's diagnostics carry in this run: one rule can detect several
            // weaknesses (a stack write and a read), and its tags must not depend on which
            // diagnostic comes first.
            std::set<unsigned> cwes;
        };

        std::vector<SarifRuleEntry> rules;
        std::unordered_map<std::string, std::size_t> ruleIndices;
        for (const auto& d : result.diagnostics)
        {
            const std::string rid = resolveRuleId(d);
            const auto [it, inserted] = ruleIndices.emplace(rid, rules.size());
            if (inserted)
                rules.push_back({rid, {}});
            if (const std::optional<unsigned> cwe = parseCweNumber(d.cweId))
                rules[it->second].cwes.insert(*cwe);
        }
        std::sort(rules.begin(), rules.end(),
                  [](const SarifRuleEntry& lhs, const SarifRuleEntry& rhs)
                  { return lhs.id < rhs.id; });

        std::ostringstream os;
        os << "{\n";
        os << "  \"version\": \"2.1.0\",\n";
        os << "  \"$schema\": "
              "\"https://schemastore.azurewebsites.net/schemas/json/sarif-2.1.0.json\",\n";
        os << "  \"runs\": [\n";
        os << "    {\n";
        os << "      \"tool\": {\n";
        os << "        \"driver\": {\n";
        os << "          \"name\": \"" << jsonEscape(toolName) << "\",\n";
        os << "          \"version\": \"" << jsonEscape(toolVersion) << "\",\n";
        os << "          \"rules\": [\n";
        for (std::size_t i = 0; i < rules.size(); ++i)
        {
            const auto& rule = rules[i];
            os << "            {\n";
            os << "              \"id\": \"" << jsonEscape(rule.id) << "\",\n";
            os << "              \"shortDescription\": { \"text\": \"" << jsonEscape(rule.id)
               << "\" }";
            if (!rule.cwes.empty())
            {
                // One score for the whole rule: the highest of its CWEs.
                unsigned severity = 0;
                for (const unsigned cwe : rule.cwes)
                    severity = std::max(severity, securitySeverityTenths(cwe));

                os << ",\n";
                os << "              \"properties\": {\n";
                os << "                \"tags\": [";
                const char* separator = "";
                if (severity > 0)
                {
                    os << "\"security\"";
                    separator = ", ";
                }
                for (const unsigned cwe : rule.cwes)
                {
                    os << separator << "\"" << cweTag(cwe) << "\"";
                    separator = ", ";
                }
                os << "]";
                if (severity > 0)
                {
                    os << ",\n                \"security-severity\": \"" << severity / 10 << "."
                       << severity % 10 << "\"";
                }
                os << "\n";
                os << "              }\n";
            }
            else
            {
                os << "\n";
            }
            os << "            }";
            if (i + 1 < rules.size())
                os << ",";
            os << "\n";
        }
        os << "          ]\n";
        os << "        }\n";
        os << "      },\n";
        os << "      \"results\": [\n";

        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            const auto& d = result.diagnostics[i];
            os << "        {\n";
            const std::string ruleId = resolveRuleId(d);
            os << "          \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";
            os << "          \"level\": \"" << severityToSarifLevel(d.severity) << "\",\n";
            os << "          \"message\": { \"text\": \"" << jsonEscape(d.message) << "\" },\n";
            bool hasConfidence = d.confidence >= 0.0;
            bool hasCwe = !d.cweId.empty();
            if (hasConfidence || hasCwe)
            {
                os << "          \"properties\": {\n";
                bool needComma = false;
                if (hasConfidence)
                {
                    os << "            \"confidence\": " << formatConfidence(d.confidence);
                    needComma = true;
                }
                if (hasCwe)
                {
                    if (needComma)
                        os << ",\n";
                    os << "            \"cwe\": \"" << jsonEscape(d.cweId) << "\"";
                }
                os << "\n";
                os << "          },\n";
            }
            os << "          \"locations\": [\n";
            os << "            {\n";
            os << "              \"physicalLocation\": {\n";
            std::string diagFilePath = d.filePath.empty() ? inputFile : d.filePath;
            std::string uriPath = stripBase(diagFilePath, baseDir);
            // SARIF requires 1-based locations. Some diagnostics can keep 0 as
            // "unknown" internally; clamp here for schema compliance.
            const unsigned sarifStartLine = d.line > 0 ? d.line : 1;
            const unsigned sarifStartColumn = d.column > 0 ? d.column : 1;
            os << "                \"artifactLocation\": { \"uri\": \"" << jsonEscape(uriPath)
               << "\" },\n";
            os << "                \"region\": {\n";
            os << "                  \"startLine\": " << sarifStartLine << ",\n";
            os << "                  \"startColumn\": " << sarifStartColumn << "\n";
            os << "                }\n";
            os << "              }\n";
            os << "            }\n";
            os << "          ]\n";
            os << "        }";
            if (i + 1 < result.diagnostics.size())
                os << ",";
            os << "\n";
        }

        os << "      ]\n";
        os << "    }\n";
        os << "  ]\n";
        os << "}\n";

        return os.str();
    }

    std::optional<RuleDescription> describeRule(std::string_view ruleId)
    {
        struct DescribedRule
        {
            std::string_view ruleId;
            RuleDescription description;
        };
        // Sorted by id. Every id the emitters can give, the error code names included: a
        // diagnostic without an explicit id takes the name of its code.
        static constexpr DescribedRule kRules[] = {
            {"AllocaTooLarge",
             {"Large stack allocation", "An alloca or variable-length array larger than the "
                                        "configured threshold, which may overflow the stack."}},
            {"AllocaUsageWarning",
             {"Dynamic stack allocation", "An alloca whose size is known only at run time: the "
                                          "stack usage of the function grows with that value."}},
            {"AllocaUserControlled",
             {"User-controlled stack allocation size",
              "An alloca or variable-length array whose size depends on user-controlled input, "
              "which may exhaust the stack."}},
            {"CommandInjection",
             {"Command injection",
              "A command that is not a literal reaches a function that runs it."}},
            {"CommandInjection.NonLiteralCommand",
             {"Non-literal command",
              "A command that is not a string literal reaches a function that runs it (system, "
              "popen, an exec function), so that data may choose what runs."}},
            {"ConstParameterNotModified",
             {"Parameter could be const", "A pointer or reference parameter that is never used to "
                                          "modify the object it designates."}},
            {"ConstParameterNotModified.Pointer",
             {"Pointer parameter could point to const",
              "A pointer parameter that is never used to modify the object it points to: it could "
              "point to const."}},
            {"ConstParameterNotModified.PointerConstOnly",
             {"Const pointer parameter could point to const",
              "A pointer parameter declared const whose pointed object is never modified: the "
              "pointed type could be const too."}},
            {"ConstParameterNotModified.Reference",
             {"Reference parameter could be const",
              "A reference parameter that is never used to modify the object it refers to: it "
              "could be a reference to const."}},
            {"ConstParameterNotModified.ReferenceRvaluePreferValue",
             {"Rvalue reference parameter could be a value",
              "An rvalue reference parameter that is never used to modify the object it refers to: "
              "it could be passed by value or by reference to const."}},
            {"DuplicateIfCondition",
             {"Duplicate if condition", "An else-if branch whose condition is equivalent to a "
                                        "previous one, so that it can never run."}},
            {"FormatString.NonLiteral",
             {"Non-literal format string",
              "A format string that is not a literal reaches a printf-like function, so that data "
              "may control the format."}},
            {"GlobalReadBeforeWrite",
             {"Global buffer read before its first write",
              "A global buffer that a function reads before it first writes it: the read sees the "
              "static zero initialization or a stale value."}},
            {"InformationExposure.UninitializedStackBytes",
             {"Uninitialized stack bytes exposed",
              "A local variable whose bytes are not all initialized reaches an external sink, "
              "which may leak memory contents."}},
            {"IntegerConversion.SignedToSize",
             {"Signed value converted to a size",
              "A signed value that may be negative is converted to the size of an allocation or a "
              "copy."}},
            {"IntegerOverflow", {"Integer overflow", "An integer computation that may overflow."}},
            {"IntegerOverflow.SignedArithmetic",
             {"Signed integer overflow",
              "A signed arithmetic operation that may overflow, which is undefined behavior."}},
            {"IntegerOverflow.SizeComputation",
             {"Integer overflow in a size computation",
              "The size of an allocation or a copy is computed with an operation that may overflow "
              "before the size is used."}},
            {"IntegerTruncation.SizeComputation",
             {"Integer truncation in a size computation",
              "The size of an allocation or a copy may be truncated to a narrower type before it "
              "is used."}},
            {"InvalidBaseReconstruction",
             {"Invalid base reconstruction",
              "A pointer to an object rebuilt from a pointer to one of its members with an offset "
              "that does not lead back to the object, which is undefined behavior."}},
            {"MemcpyWithStackDest",
             {"Stack buffer overflow in a memory copy",
              "A memcpy, memset or similar copy whose length may exceed the stack buffer it "
              "writes."}},
            {"MultipleStoresToStackBuffer",
             {"Multiple stores to a stack buffer",
              "A stack buffer written many times in one function: each index is worth checking "
              "against the buffer's bounds."}},
            {"NegativeStackIndex",
             {"Negative stack index", "An index into a stack array that may be negative, so that "
                                      "the access lands before the array."}},
            {"NullPointerDereference",
             {"Null pointer dereference", "A pointer that may be null is dereferenced."}},
            {"OutOfBoundsRead", {"Out-of-bounds read", "A read past the end of a buffer."}},
            {"OutOfBoundsRead.HeapIndex",
             {"Out-of-bounds read on a heap buffer",
              "A read at an index that may be past the end of a heap buffer."}},
            {"OutOfBoundsRead.MissingNullTerminator",
             {"String buffer without a null terminator",
              "A string function reads a buffer that may not be null-terminated, past its end."}},
            {"Recursion.Detected",
             {"Recursive function", "A recursive or mutually recursive function: its stack usage "
                                    "grows with the depth of the recursion."}},
            {"Recursion.Unconditional",
             {"Unconditional recursion", "A function that calls itself on every path, with no base "
                                         "case: it overflows the stack at run time."}},
            {"ResourceLifetime.AnalysisIncomplete",
             {"Resource analysis incomplete", "The resource ownership analysis did not converge in "
                                              "this function: its resources are not checked."}},
            {"ResourceLifetime.DoubleRelease",
             {"Double release", "A resource handle released twice, such as a file closed or memory "
                                "freed a second time."}},
            {"ResourceLifetime.IncompleteInterproc",
             {"Inter-procedural resource analysis incomplete",
              "A handle passed to a function the analysis could not follow: whether it is released "
              "is not known."}},
            {"ResourceLifetime.MissingDestructorRelease",
             {"Resource acquired in a constructor may leak",
              "A resource a constructor acquires and the class's destructor does not release."}},
            {"ResourceLifetime.MissingRelease",
             {"Resource leak",
              "A resource acquired in a function and not released on every path out of it."}},
            {"ResourceLifetime.ReleasedHandleEscapes",
             {"Released handle escapes",
              "A handle that was released is returned or stored, so that it may be used later."}},
            {"ResourceLifetime.UseAfterRelease",
             {"Use after release", "A resource handle used after it was released."}},
            {"ResourceLifetimeIssue",
             {"Resource lifetime issue",
              "A resource that may leak, be released twice or be used after it was released."}},
            {"SizeMinusOneWrite",
             {"Size minus a constant as a length",
              "A length computed as a size minus a constant, which may fall below the minimum of "
              "its type before a write or a call."}},
            {"SizeofPitfall",
             {"Suspicious sizeof",
              "A sizeof applied to a pointer or an array parameter, which gives the size of the "
              "pointer, where the size of the data was likely meant."}},
            {"StackBufferOverflow",
             {"Stack buffer overflow",
              "A read or a write at an index that may be past the end of a stack array."}},
            {"StackFrameTooLarge",
             {"Stack frame too large",
              "A function whose stack frame exceeds the configured limit."}},
            {"StackPointerEscape",
             {"Stack address escapes", "The address of a local variable escapes its function, so "
                                       "that it may be used after the stack frame is gone."}},
            {"TOCTOU.PathCheckThenUse",
             {"Path checked then used", "A path checked (access, stat) and then used (open) by "
                                        "separate calls: the file may change in between."}},
            {"TOCTOURace",
             {"Time-of-check to time-of-use race", "A resource checked and then used by separate "
                                                   "operations: it may change in between."}},
            {"TypeConfusion",
             {"Type confusion", "The same memory accessed through incompatible types."}},
            {"TypeConfusion.IncompatibleStructView",
             {"Incompatible struct views of one pointer",
              "The same pointer used as two incompatible struct types."}},
            {"UninitializedLocalRead",
             {"Read of an uninitialized local variable",
              "A local variable read before it is written on some path."}},
            {"UninitializedLocalRead.AnalysisIncomplete",
             {"Uninitialized-read analysis incomplete",
              "The uninitialized-read analysis did not converge in this function: its reads are "
              "not checked."}},
            {"UninitializedLocalVariable",
             {"Uninitialized local variable",
              "A local variable declared without an initializer and never definitely written."}},
            {"UnsafeFunction.DeprecatedGets",
             {"Use of gets",
              "A call to gets, which cannot bound what it writes and was removed from C11."}},
            {"VLAUsage",
             {"Variable-length array",
              "A stack allocation whose size is not a compile-time constant."}},
            {"VariadicFormatMismatch",
             {"Format and arguments mismatch",
              "The arguments of a printf-like call do not match its format string."}},
        };
        const auto found = std::lower_bound(std::begin(kRules), std::end(kRules), ruleId,
                                            [](const DescribedRule& rule, std::string_view id)
                                            { return rule.ruleId < id; });
        if (found == std::end(kRules) || found->ruleId != ruleId)
            return std::nullopt;
        return found->description;
    }
} // namespace ctrace::stack
