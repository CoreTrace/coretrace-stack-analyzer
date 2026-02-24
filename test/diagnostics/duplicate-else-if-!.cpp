#include <iostream>
#include <type_traits>

typedef struct cfg_s
{
    bool isMangled = false;
} cfg_t;

int main(void)
{
    cfg_t cfg = {1};

    if (cfg.isMangled)
    {
        printf("Is mangled\n");
    }
    else if (!cfg.isMangled)
    {
        printf("Is not mangled\n");
    }

    if (!cfg.isMangled)
    {
        printf("Is mangled\n");
    }
    // at line 29, column 14
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (!cfg.isMangled)
    {
        printf("Is not mangled\n");
    }

    if (cfg.isMangled == false)
    {
        printf("Is mangled\n");
    }
    // at line 41, column 14
    // [ !!Warn ] unreachable else-if branch: condition is equivalent to a previous 'if' condition
    //          ↳ else branch implies previous condition is false
    else if (!cfg.isMangled)
    {
        printf("Is not mangled\n");
    }

    if (cfg.isMangled == true)
    {
        printf("Is mangled\n");
    }
    else if (!cfg.isMangled == true)
    {
        printf("Is not mangled\n");
    }

    return 0;
}
