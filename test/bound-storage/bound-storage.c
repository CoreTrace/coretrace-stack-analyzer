int main(void)
{
    char test[10];

    // [!!] potential stack buffer overflow on variable '<test>' (size 10)
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    test[11] = 'a';

    test[9] = 'b';  // OK

    // [!!] potential stack buffer overflow on variable '<test>' (size 10)
    //     constant index 18446744073709551615 is out of bounds (0..9)
    //     (this is a write access)
    test[-1] = 'c';

    test[11 - 2] = 'd';  // OK

    return 0;
}
