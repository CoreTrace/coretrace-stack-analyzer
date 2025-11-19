int foo(void)
{
    char test[8192000000];
    return 0;
}

int bar(void)
{
    return 0;
}

int main(void)
{
    foo();
    bar();

    return 0;
}
// total(main) = local(main) + max( total(foo), total(bar) )
// 32 = 16 + 16
