// [!Info] multiple stores to stack buffer 'test' in this function (4 store instruction(s), 3 distinct index expression(s))
//     stores use different index expressions; verify indices are correct and non-overlapping
int main(void)
{
    char test[10];

    // at line 12, column 14
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    test[11] = 'a';

    test[9] = 'b';  // OK

    // at line 21, column 14
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     constant index 18446744073709551615 is out of bounds (0..9)
    //     (this is a write access)
    test[-1] = 'c';

    test[11 - 2] = 'd';  // OK

    return 0;
}
