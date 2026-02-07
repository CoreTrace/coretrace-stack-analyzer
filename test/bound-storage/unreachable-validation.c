void unreachable_validation_local_const(void)
{
    int i = 11;
    char test[10];

    // at line 14, column 18
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    //     [info] this access appears unreachable at runtime (condition is always false for this branch)

    if (i <= 10)
        test[11] = 'a';
}
