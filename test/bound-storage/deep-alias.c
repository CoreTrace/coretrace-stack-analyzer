void deep_alias(char *src)
{
    char buf[10];
    char *p1 = buf;
    char *p2 = p1;
    char **pp = &p2;

    // at line 14, column 18
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf -> arraydecay -> p1 -> p2 -> pp
    //     index variable may go up to 19 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i < 20; ++i) {
        (*pp)[i] = src[i];
    }
}

int main(void)
{
    // at line 23, column 10
    // [!!] stack pointer escape: address of variable 'src' escapes this function
    //     address passed as argument to function 'llvm.memset.p0.i64' (callee may capture the pointer beyond this function)
    char src[20] = {0};
    // at line 27, column 5
    // [!!] stack pointer escape: address of variable 'src' escapes this function
    //     address passed as argument to function 'deep_alias' (callee may capture the pointer beyond this function)
    deep_alias(src);
    return 0;
}
