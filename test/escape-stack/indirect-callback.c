typedef void (*cb_t)(char*);

void use_callback(cb_t cb)
{
    char buf[10];

    cb(buf); // potential leak by callback
}

// at line 5, column 1
// [ !!Warn ] local variable 'buf' is never initialized
//          ↳ declared without initializer and no definite write was found in this function

// at line 7, column 5
// [ !!Warn ] stack pointer escape: address of variable 'buf' escapes this function
//          ↳ address passed as argument to an indirect call (callback may capture the pointer beyond this function)
