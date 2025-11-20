int main(void)
{
    char test[10];

    // [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
    //    index variable may go up to 11 (array last valid index: 9)
    //    (this is a write access)
    for (int i = 0; i < 20; i++) {
        test[i] = 'a';
    }

    // [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
    //    index variable may go up to 19 (array last valid index: 9)
    //    (this is a write access)
    for (int i = 0; i != 11; ++i)
        test[i] = 'a';

    // OK
    for (int i = 0; i < 10; i++) {
        test[i] = 'b';
    }

    return 0;
}
