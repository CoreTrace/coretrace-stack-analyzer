int lambda_receiver_object_should_not_warn_never_initialized(void)
{
    auto buildCanonicalize = [&](int v) { return v + 1; };

    return buildCanonicalize(41);
}

// not contains: local variable 'buildCanonicalize' is never initialized
