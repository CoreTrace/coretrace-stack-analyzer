// not contains: unreachable else-if branch: condition is equivalent to a previous 'if' condition
int classify_alloca_case(bool sizeIsConst, unsigned long long sizeBytes, bool hasUpperBound,
                         unsigned long long upperBoundBytes,
                         unsigned long long allocaLargeThreshold, unsigned long long stackLimit)
{
    bool isOversized = false;

    if (sizeIsConst && sizeBytes >= allocaLargeThreshold)
        isOversized = true;
    else if (hasUpperBound && upperBoundBytes >= allocaLargeThreshold)
        isOversized = true;
    else if (sizeIsConst && stackLimit != 0 && sizeBytes >= stackLimit)
        isOversized = true;

    return isOversized ? 1 : 0;
}

int main()
{
    return classify_alloca_case(true, 48ull * 1024ull, false, 0, 64ull * 1024ull, 32ull * 1024ull);
}
