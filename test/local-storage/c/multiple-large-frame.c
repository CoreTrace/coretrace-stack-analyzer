int main(void)
{
    // stack-limit: 30
    // at line 13, column 5
    // [!] potential stack overflow: exceeds limit of 30 bytes
    //      locals: 5 variables (total 32 bytes)
    //      locals list: a(4), b(4), c(4), d(4), retval(4)
    int a;
    int b;
    int c;
    int d;

    return 0;
}
