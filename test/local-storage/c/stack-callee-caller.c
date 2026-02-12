int foo(void)
{
    // local stack: 8192000000 bytes
    // max stack (including callees): 8192000000 bytes
    // at line 9, column 5
    // [!!!Error] potential stack overflow: exceeds limit of 8388608 bytes
    //          ↳ alias variable: test
    char test[8192000000];
    return 0;
}

int bar(void)
{
    // local stack: 0 bytes
    // at line 18, column 5
    // [!!!Error] potential stack overflow: exceeds limit of 8388608 bytes
    //          ↳ path: bar -> foo
    foo();
    return 0;
}

int mano(void)
{
    // local stack: 0 bytes
    // max stack (including callees): 8192000000 bytes
    // at line 29, column 5
    // [!!!Error] potential stack overflow: exceeds limit of 8388608 bytes
    //          ↳ path: mano -> bar -> foo
    bar();

    return 0;
}
// total(main) = local(main) + max( total(foo), total(bar) )
// 32 = 16 + 16

// at line 8, column 1
// [ !!Warn ] local variable 'test' is never initialized
//          ↳ declared without initializer and no definite write was found in this function
