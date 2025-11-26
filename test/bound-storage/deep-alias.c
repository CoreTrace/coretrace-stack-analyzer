void deep_alias(char *src)
{
    char buf[10];
    char *p1 = buf;
    char *p2 = p1;
    char **pp = &p2;

    for (int i = 0; i < 20; ++i) {
        (*pp)[i] = src[i];
    }
}

int main(void)
{
    char src[20] = {0};
    deep_alias(src);
    return 0;
}
