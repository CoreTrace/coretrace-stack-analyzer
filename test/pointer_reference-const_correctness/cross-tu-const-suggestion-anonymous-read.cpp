// SPDX-License-Identifier: Apache-2.0
// A function in an anonymous namespace: no other file can name it, although its homonym in another
// file has the same mangled name. That homonym must not refute this one's suggestion.
namespace
{
int helper(int* p)
{
    return *p;
}
} // namespace

int read_through_helper()
{
    int value = 1;
    return helper(&value);
}
