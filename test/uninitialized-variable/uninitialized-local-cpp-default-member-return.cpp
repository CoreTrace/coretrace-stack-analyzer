// SPDX-License-Identifier: Apache-2.0
#include <cstdint>
#include <string>

struct StorageKeyLike
{
    int scope = 0;
    std::string key = "";
    std::string displayName = "";
    std::uint64_t offset = 0;
    int argumentIndex = -1;
    void* localAlloca = nullptr;
};

static StorageKeyLike build_storage_key_like(bool earlyA, bool earlyB)
{
    StorageKeyLike out;
    if (earlyA)
        return out;
    if (earlyB)
        return out;

    out.scope = 1;
    out.key = "ok";
    return out;
}

int default_member_record_return_paths_should_not_warn(bool a, bool b)
{
    StorageKeyLike value = build_storage_key_like(a, b);
    return value.scope;
}

// not contains: potential read of uninitialized local variable 'out'
