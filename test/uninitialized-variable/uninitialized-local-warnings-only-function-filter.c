static int read_uninitialized_value(void)
{
    int value;
    return value;
}

static int clean_value(void)
{
    return 7;
}

int main(void)
{
    return read_uninitialized_value() + clean_value();
}
