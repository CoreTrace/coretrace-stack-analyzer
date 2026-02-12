void foo(void)
{
    int n = 6;
    char buf[n]; // technically a VLA, but bounded and trivial, patch for false positive
}

int main(int ac, char** av)
{
    foo();
    return 0;
}

// at line 4, column 5
// [!] dynamic alloca on the stack for variable 'vla'
//     allocation performed via alloca/VLA; stack usage grows with runtime value
//     requested stack size: 6 bytes
//     size does not appear user-controlled but remains runtime-dependent