int main(void)
{
    char test[10];
    char *ptr = test;
    char **pp = &ptr;
    (ptr)[14] = 'a';
    (*pp)[15] = 'a';

    // [!!] potential stack buffer overflow on variable '<test>' (size 10)
    //    index variable may go up to 19 (array last valid index: 9)
    //    (this is a write access)
    for (int i = 0; i < 20; i++) {
        test[i] = 'a';
    }

    // [!!] potential stack buffer overflow on variable '<test>' (size 10)
    //    index variable may go up to 11 (array last valid index: 9)
    //    (this is a write access)
    for (int i = 0; i != 11; ++i)
        test[i] = 'a';

    // OK
    for (int i = 0; i < 10; i++) {
        test[i] = 'b';
    }

    // Same for pointer aliasing
    // [!!] potential stack buffer overflow on variable '<unnamed>' (size 10)
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; i++) {
        ptr[i] = 'a';
    }

    int n = 6;
    char buf[n];   // alloca variable
    return 0;
}
