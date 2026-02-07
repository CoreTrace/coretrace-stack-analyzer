#include "StackUsageAnalyzer.hpp"

#include <cstdio> // std::snprintf
#include <sstream>
#include <string>
#include <vector>

namespace ctrace::stack
{
    namespace
    {

        // Petit helper pour échapper les chaînes JSON.
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
                        char buf[7];
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

    } // anonymous namespace

    static std::string toJsonImpl(const AnalysisResult& result, const std::string* inputFile,
                                  const std::vector<std::string>* inputFiles)
    {
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

        // Fonctions
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
            os << "      \"maxStackUnknown\": " << (f.maxStackUnknown ? "true" : "false")
               << ",\n";
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
            const std::string ruleId =
                d.ruleId.empty() ? std::string(ctrace::stack::enumToString(d.errCode)) : d.ruleId;
            os << "      \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";

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
        os << "  ]\n";
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
                        const std::string& toolName, const std::string& toolVersion)
    {
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
        os << "          \"version\": \"" << jsonEscape(toolVersion) << "\"\n";
        os << "        }\n";
        os << "      },\n";
        os << "      \"results\": [\n";

        for (std::size_t i = 0; i < result.diagnostics.size(); ++i)
        {
            const auto& d = result.diagnostics[i];
            os << "        {\n";
            // Pour le moment, un seul ruleId générique; tu pourras le spécialiser plus tard.
            const std::string ruleId =
                d.ruleId.empty() ? std::string(ctrace::stack::enumToString(d.errCode)) : d.ruleId;
            os << "          \"ruleId\": \"" << jsonEscape(ruleId) << "\",\n";
            os << "          \"level\": \"" << severityToSarifLevel(d.severity) << "\",\n";
            os << "          \"message\": { \"text\": \"" << jsonEscape(d.message) << "\" },\n";
            os << "          \"locations\": [\n";
            os << "            {\n";
            os << "              \"physicalLocation\": {\n";
            std::string diagFilePath = d.filePath.empty() ? inputFile : d.filePath;
            os << "                \"artifactLocation\": { \"uri\": \"" << jsonEscape(diagFilePath)
               << "\" },\n";
            os << "                \"region\": {\n";
            os << "                  \"startLine\": " << d.line << ",\n";
            os << "                  \"startColumn\": " << d.column << "\n";
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
