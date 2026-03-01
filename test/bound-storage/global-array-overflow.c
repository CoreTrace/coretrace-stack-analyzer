int gbuf[10];

int main(void)
{
    int i = 0;
    for (; i < 10; ++i)
    {
        gbuf[i] = i;
    }

    // at line 17, column 13
    // [ !!Warn ] potential buffer overflow on global variable 'gbuf' (size 10)
    // ↳ alias path: gbuf
    // ↳ index variable may go up to 10 (array last valid index: 9)
    // ↳ (this is a write access)
    // ↳ [info] this access appears unreachable at runtime (condition is always false for this branch)
    gbuf[i] = i;

    return gbuf[0];
}
