enum class InitState
{
    Uninit,
    Partial,
    Init
};

int fp_duplicate_if_unreachable_elseif_guard(bool isCtorThis, bool isAssignmentLike,
                                             InitState state)
{
    bool hasReadBeforeWrite = false;
    if (state == InitState::Uninit)
        hasReadBeforeWrite = true;

    if (hasReadBeforeWrite)
    {
        const bool suppressForAssignmentPadding = isAssignmentLike && state == InitState::Partial;
        const bool suppressCtorThisReadBeforeWrite = isCtorThis && state == InitState::Uninit;
        if (suppressForAssignmentPadding || suppressCtorThisReadBeforeWrite)
            return 1;
    }

    return 0;
}

// strict-diagnostic-count: false
// not contains: unreachable else-if branch: condition is equivalent to a previous 'if' condition
