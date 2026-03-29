// SPDX-License-Identifier: Apache-2.0
struct AnalysisConfig
{
    bool quiet = false;
    bool warningsOnly = false;
    unsigned jobs = 0;
};

struct ParsedArguments
{
    AnalysisConfig config = {};
    bool verbose = false;
};

struct ParseResult
{
    ParsedArguments parsed = {};
    int status = 0;
};

int nested_subobject_projection_should_not_warn(void)
{
    ParseResult result{};
    ParsedArguments& parsed = result.parsed;
    AnalysisConfig& cfg = parsed.config;

    cfg.quiet = false;
    cfg.warningsOnly = true;
    cfg.jobs = 2;

    return static_cast<int>(result.parsed.config.jobs);
}

// not contains: potential UB: invalid base reconstruction via offsetof/container_of
// not contains: unable to verify that derived pointer points to a valid object
