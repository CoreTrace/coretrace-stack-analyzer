// int main(void)
// {
//     int i = 1;
//     char test[10];

//     if (i > 10) {
//         test[11] = 'a';
//     }
//     char test1[10]; if (i <= 10) test1[i] = 'a';
//     return 0;
// }

//   [warn] multiple stores to stack buffer '<unnamed>' in this function (2 store instruction(s), 2 distinct index expression(s))
//        stores use different index expressions; verify indices are correct and non-overlapping
int main(void)
{
    int i = 11;
    char test[10];

//   [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
//        constant index 11 is out of bounds (0..9)
//        (this is a write access)
//        [info] this access appears unreachable at runtime (condition is always false for this branch)
    if (i <= 10)
        test[11] = 'a';

    // [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
    //    index variable may go up to 10 (array last valid index: 9)
    //    (this is a write access)
    //    [info] this access appears unreachable at runtime (condition is always false for this branch)
    char test1[10];
    if (i <= 10)
        test1[i] = 'a';

//   [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
//        index variable may go up to 10 (array last valid index: 9)
//        (this is a write access)
    char test2[10];
    if (i > 10)
        test1[i] = 'a';

    return 0;
}
