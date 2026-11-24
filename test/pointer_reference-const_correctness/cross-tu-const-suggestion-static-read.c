// SPDX-License-Identifier: Apache-2.0
// A static function: no other file can name it. A static homonym in another file is another
// function, and must not refute this one's suggestion.
static int helper(int* p)
{
    return *p;
}

int read_through_helper(void)
{
    int value = 1;
    return helper(&value);
}
