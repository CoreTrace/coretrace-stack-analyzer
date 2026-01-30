int main(void)
{
    char test[10];
    char* ptr = test;

    // at line 12, column 17
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; i++)
    {
        test[i] = 'a';
    }

    // at line 21, column 17
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     index variable may go up to 11 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i != 11; ++i)
        test[i] = 'a';

    // OK
    for (int i = 0; i < 10; i++)
    {
        test[i] = 'b';
    }

    // at line 34, column 16
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test -> arraydecay -> ptr
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; i++)
    {
        ptr[i] = 'a';
    }

    // [!Info] multiple stores to stack buffer 'test' in this function (4 store instruction(s), 4 distinct index expression(s))
    //     stores use different index expressions; verify indices are correct and non-overlapping
    int n = 6;
    char buf[n]; // alloca variable
    return 0;
}
