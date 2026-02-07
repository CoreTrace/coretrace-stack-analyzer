
void unreachable_multi_pred_mixed(int x)
{
    int zero = 0;
    char test[10];

    // at line 19, column 14
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    if (zero)
        goto L;
    if (x > 0)
        goto L;
    return;

L:
    test[11] = 'a';
}

// not contains: [info] this access appears unreachable at runtime (condition is always false for this branch)
