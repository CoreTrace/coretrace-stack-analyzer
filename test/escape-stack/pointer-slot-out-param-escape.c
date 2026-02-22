void leak_pointer_slot(int*** out)
{
    int* p = 0;
    *out = &p;
}

// at line 4, column 10
// [ !!Warn ] stack pointer escape: address of variable 'p' escapes this function
//          ↳ stored through a non-local pointer (e.g. via an out-parameter; pointer may outlive this function)
