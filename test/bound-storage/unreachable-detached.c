// SPDX-License-Identifier: Apache-2.0
void unreachable_detached_region(void)
{
    int i = 11;
    char test[10];

    goto done;

    // dead code
    if (i <= 10)
        test[11] = 'a';

done:
    return;
}

// at line 4, column 1
// [ !!Warn ] local variable 'test' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
