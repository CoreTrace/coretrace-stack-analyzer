void ne_must_not_narrow_range(int i)
{
    char buf[200];

    // at line 13, column 20
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 200)
    // ↳ alias path: buf
    // ↳ index variable may go up to 301 (array last valid index: 199)
    // ↳ (this is a write access)
    if (i > 300)
    {
        if (i != 100)
            buf[i] = 1;
    }
}

int main(void)
{
    ne_must_not_narrow_range(400);
    return 0;
}
