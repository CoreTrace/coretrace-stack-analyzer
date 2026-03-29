// SPDX-License-Identifier: Apache-2.0
#include <iostream>

void foo(void)
{
    std::string suffix = "test";
    bool prefix = false;

    if (suffix.empty())
        return;
    // at line 13, column 21
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (suffix.empty())
        return;

    if (prefix)
        return;
    // at line 21, column 14
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (prefix)
        return;
}

int main(void)
{
    foo();

    return 0;
}
