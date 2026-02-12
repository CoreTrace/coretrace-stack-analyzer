int main(void)
{
    char test[10];
    char* ptr = test;

    for (int i = 0; i < 20; i++)
    {
        test[i] = 'a';
    }

    for (int i = 0; i != 11; ++i)
        test[i] = 'a';

    // OK
    for (int i = 0; i < 10; i++)
    {
        test[i] = 'b';
    }

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

// at line 8, column 17
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ index variable may go up to 19 (array last valid index: 9)
// ↳ (this is a write access)

// at line 12, column 17
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ index variable may go up to 11 (array last valid index: 9)
// ↳ (this is a write access)

// at line 22, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test -> arraydecay -> ptr
// ↳ index variable may go up to 19 (array last valid index: 9)
// ↳ (this is a write access)

// at line 28, column 5
// [ !!Warn ] dynamic alloca on the stack for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ requested stack size: 6 bytes
// ↳ size does not appear user-controlled but remains runtime-dependent

// [!Info!] multiple stores to stack buffer 'test' in this function (4 store instruction(s), 4 distinct index expression(s))
// [!Info!] stores use different index expressions; verify indices are correct and non-overlapping