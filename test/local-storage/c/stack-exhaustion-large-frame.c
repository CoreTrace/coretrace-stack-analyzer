#define SIZE_LARGE 8192000000
#define SIZE_SMALL (SIZE_LARGE / 2)

int main(void)
{
    // local stack: 4096000016 bytes
    // max stack (including callees): 4096000016 bytes
    // at line 13, column 5
    // [!] potential stack overflow: exceeds limit of 8388608 bytes
    //     alias path: test
    char test[SIZE_LARGE];

    return 0;
}
