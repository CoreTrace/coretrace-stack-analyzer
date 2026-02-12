char* ret_buf(void)
{
    char buf[10];

    return buf; // warning expected: return
}

int main(void)
{
    char* p = ret_buf();
    (void)p;
    return 0;
}

// at line 3, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 5, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)