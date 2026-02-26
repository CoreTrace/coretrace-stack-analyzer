class TrivialAnalyzerApp
{
  public:
    int run() const
    {
        return 7;
    }
};

int trivial_ctor_object_should_not_warn_never_initialized(void)
{
    TrivialAnalyzerApp app;
    return app.run();
}

// not contains: local variable 'app' is never initialized
// not contains: potential read of uninitialized local variable 'app'
