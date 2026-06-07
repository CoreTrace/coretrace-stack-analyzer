// SPDX-License-Identifier: Apache-2.0
// strict-diagnostic-count: false

struct ViewLike
{
    const char* data;
    unsigned long size;

    constexpr bool ends_with(ViewLike suffix) const
    {
        return size >= suffix.size && suffix.data != nullptr;
    }
};

int range_for_pointer_slot_induction_should_not_warn(ViewLike filename)
{
    constexpr ViewLike cExtensions[] = {{".c", 2}, {".h", 2}};

    for (const auto& ext : cExtensions)
    {
        if (filename.ends_with(ext))
            return 1;
    }

    return 0;
}

int main(void)
{
    return range_for_pointer_slot_induction_should_not_warn({"main.c", 6});
}

// not contains: potential UB: invalid base reconstruction via offsetof/container_of
// not contains: derived pointer points OUTSIDE the valid object range
