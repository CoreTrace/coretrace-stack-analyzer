// SPDX-License-Identifier: Apache-2.0
#include <iostream>

struct Config
{
    bool isMangled = false;
};

int main()
{
    Config cfg;

    if (cfg.isMangled)
    {
        std::cout << "Is mangled\n";
    }
    // at line 19, column 28
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (cfg.isMangled == false)
    {
        std::cout << "Is not mangled\n";
    }

    return 0;
}
