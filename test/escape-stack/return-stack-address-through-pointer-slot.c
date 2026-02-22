char* leak_through_slot(void)
{
    char buf[8];
    char* p = buf;
    return p;
}

// at line 5, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)
