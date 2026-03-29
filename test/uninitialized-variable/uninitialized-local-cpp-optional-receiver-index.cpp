// SPDX-License-Identifier: Apache-2.0
#include <optional>

static std::optional<unsigned> getMethodReceiverIdx(bool hasReceiver)
{
    if (hasReceiver)
        return 0u;
    return std::nullopt;
}

static bool argMayWriteThrough(unsigned argIdx, std::optional<unsigned> methodReceiverIdx)
{
    if (methodReceiverIdx && argIdx == *methodReceiverIdx)
        return false;
    return true;
}

int optional_receiver_index_should_not_warn(bool hasReceiver)
{
    const std::optional<unsigned> methodReceiverIdx = getMethodReceiverIdx(hasReceiver);
    int writes = 0;
    for (unsigned argIdx = 0; argIdx < 3; ++argIdx)
    {
        if (!argMayWriteThrough(argIdx, methodReceiverIdx))
            continue;
        ++writes;
    }
    return writes;
}
