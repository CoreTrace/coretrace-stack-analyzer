// [!Info] multiple stores to stack buffer 'test' in this function (6 store instruction(s), 6 distinct index expression(s))
//     stores use different index expressions; verify indices are correct and non-overlapping
int main(void)
{
    char test[10];
    char* ptr = test;
    char** pp = &ptr;
    // at line 13, column 15
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test -> arraydecay -> ptr
    //     constant index 14 is out of bounds (0..9)
    //     (this is a write access)
    (ptr)[14] = 'a';
    // at line 19, column 15
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test -> arraydecay -> ptr -> pp
    //     constant index 15 is out of bounds (0..9)
    //     (this is a write access)
    (*pp)[15] = 'a';

    // at line 27, column 17
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; i++)
    {
        test[i] = 'a';
    }

    // at line 36, column 17
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

    // at line 49, column 16
    // [!!] potential stack buffer overflow on variable 'test' (size 10)
    //     alias path: test -> arraydecay -> ptr
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; i++)
    {
        ptr[i] = 'a';
    }

    int n = 6;
    char buf[n]; // alloca variable
    return 0;
}
