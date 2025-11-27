// [!Info] multiple stores to stack buffer 'test1' in this function (2 store instruction(s), 2 distinct index expression(s))
//     stores use different index expressions; verify indices are correct and non-overlapping
int main(void)
{
    int i = 11;
    char test[10];

    // at line 15, column 18
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    //     [info] this access appears unreachable at runtime (condition is always false for this branch)
    if (i <= 10)
        test[11] = 'a';

    // at line 25, column 18
    // [!!] potential stack buffer overflow on variable 'test1' (size 10)
    //     alias path: test1
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    //     [info] this access appears unreachable at runtime (condition is always false for this branch)
    char test1[10];
    if (i <= 10)
        test1[i] = 'a';

    // at line 34, column 18
    // [!!] potential stack buffer overflow on variable 'test1' (size 10)
    //     alias path: test1
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    char test2[10];
    if (i > 10)
        test1[i] = 'a';

    return 0;
}
