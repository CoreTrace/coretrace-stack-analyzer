char* ret_buf(void)
{
    char buf[10];
    // at line 7, column 5
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     escape via return statement (pointer to stack returned to caller)
    return buf; // warning attendu: return
}

int main(void)
{
    char* p = ret_buf();
    (void)p;
    return 0;
}
