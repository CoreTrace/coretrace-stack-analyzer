typedef void (*cb_t)(char*);

void use_callback(cb_t cb)
{
    char buf[10];
    // at line 9, column 5
    // [!!] stack pointer escape: address of variable 'buf' escapes this function
    //     address passed as argument to an indirect call (callback may capture the pointer beyond this function)
    cb(buf); // potential leak by callback
}
