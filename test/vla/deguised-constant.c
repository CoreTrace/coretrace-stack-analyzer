void foo(void)
{
    int n = 6;
    char buf[n];   // techniquement VLA, mais bornée et triviale
}

int main(int ac, char **av)
{
    foo();
    return 0;
}
