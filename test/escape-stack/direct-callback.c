// // case_call_arg.c
// void sink(char* p);

// void pass_to_sink(void)
// {
//     char buf[10];
//     // at line 10, column 5
//     // [!!] stack pointer escape: address of variable 'buf' escapes this function
//     //     address passed as argument to function 'sink' (callee may capture the pointer beyond this function)
//     sink(buf); // callee may capture the pointer
// }

void temporary(void)
{
    // dummy function to prevent tail call optimization of pass_to_sink()
}
