int compute_range(int input)
{
    int pred = input;
    bool hasLB = false;
    bool hasUB = false;
    long long lb = 0;
    long long ub = 0;

    auto update = [&](bool valueIsOp0)
    {
        if (valueIsOp0)
        {
            hasLB = true;
            lb = static_cast<long long>(pred);
        }
        else
        {
            hasUB = true;
            ub = static_cast<long long>(pred + 1);
        }
    };

    update(true);
    update(false);
    return pred + (hasLB ? static_cast<int>(lb) : 0) + (hasUB ? static_cast<int>(ub) : 0);
}

int main()
{
    return compute_range(3);
}

// not contains: stack pointer escape: address of variable 'pred' escapes this function
// not contains: stack pointer escape: address of variable 'hasLB' escapes this function
// not contains: stack pointer escape: address of variable 'hasUB' escapes this function
// not contains: stack pointer escape: address of variable 'lb' escapes this function
// not contains: stack pointer escape: address of variable 'ub' escapes this function
