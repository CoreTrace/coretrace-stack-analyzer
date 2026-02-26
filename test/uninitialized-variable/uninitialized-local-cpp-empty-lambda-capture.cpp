enum class MemKind
{
    None,
    A,
    B,
    C
};

int empty_lambda_captured_indirect_call_should_not_warn(void)
{
    auto classifyByName = [&](int v) -> MemKind
    {
        if (v == 1)
            return MemKind::A;
        if (v == 2)
            return MemKind::B;
        if (v == 3)
            return MemKind::C;
        return MemKind::None;
    };

    MemKind kind = [&]() -> MemKind { return classifyByName(1); }();

    return static_cast<int>(kind);
}

// not contains: local variable 'classifyByName' is never initialized
