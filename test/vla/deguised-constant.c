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
