#include "StackUsageAnalyzer.hpp"

#include <algorithm>
#include <cstdio> // std::snprintf
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
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
            std::string cweId;
        };

        std::vector<SarifRuleEntry> rules;
        std::unordered_map<std::string, std::size_t> ruleIndices;
        for (const auto& d : result.diagnostics)
        {
            const std::string rid = resolveRuleId(d);
            auto it = ruleIndices.find(rid);
            if (it == ruleIndices.end())
            {
                ruleIndices.emplace(rid, rules.size());
                rules.push_back({rid, d.cweId});
            }
            else if (rules[it->second].cweId.empty() && !d.cweId.empty())
            {
                rules[it->second].cweId = d.cweId;
            }
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
            if (!rule.cweId.empty())
            {
                os << ",\n";
                os << "              \"properties\": {\n";
                os << "                \"tags\": [\"" << jsonEscape(rule.cweId) << "\"]\n";
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

} // namespace ctrace::stack
