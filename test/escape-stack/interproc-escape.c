// static char* g_ptr;

// static void store_global(char* p)
// {
//     g_ptr = p;
// }

// void escape_via_defined_callee(void)
// {
//     char buf[10];
//     // at line 14, column 5
//     // [!!] stack pointer escape: address of variable 'buf' escapes this function
//     //     address passed as argument to function 'store_global' (callee may capture the pointer beyond this function)
//     store_global(buf);
// }

// This test checks that we can detect interprocedural escapes through defined callees, even when the callee is not marked with attributes like 'nocapture' (which is the case here since the callee is defined in the same translation unit and we don't want to rely on the pass adding 'nocapture' attributes).
// Note that this test is not about the precision of the analysis (we may have false positives, and in this case we do since 'store_global' does not actually capture the pointer beyond the function), but rather about the ability to detect that there is an escape through the call to 'store_global'.

void temporary(void)
{
    // dummy function to prevent tail call optimization of escape_via_defined_callee()
}