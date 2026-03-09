struct AnalysisConfigLike
{
    unsigned long long stackLimit = 8ull * 1024ull * 1024ull;
    bool enabled = false;
};

static int analyzeWithConfig(int input, const AnalysisConfigLike& cfg)
{
    return cfg.enabled ? input : (input + static_cast<int>(cfg.stackLimit % 7ull));
}

int fp_uninitialized_default_config_wrapper(int input)
{
    const AnalysisConfigLike defaultConfig{};
    return analyzeWithConfig(input, defaultConfig);
}

// strict-diagnostic-count: false
// not contains: potential read of uninitialized local variable 'defaultConfig'
