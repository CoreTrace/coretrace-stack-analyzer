void unreachable_detached_region(void)
{
    int i = 11;
    char test[10];

    goto done;

    if (i <= 10)
        test[11] = 'a';

done:
    return;
}
