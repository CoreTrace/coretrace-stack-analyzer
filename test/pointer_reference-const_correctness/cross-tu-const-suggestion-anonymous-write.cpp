// SPDX-License-Identifier: Apache-2.0
// A homonym of cross-tu-const-suggestion-anonymous-read.cpp's helper, in this file's anonymous
// namespace, which writes through its parameter.
namespace
{
    int helper(int* p)
    {
        *p = 2;
        return 0;
    }
} // namespace

int write_through_helper()
{
    int value = 1;
    return helper(&value);
}
