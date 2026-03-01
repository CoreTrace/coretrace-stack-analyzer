bool isPositive(int x_test)
{
    if (x_test > 0)
    {
        return true; // Condition redondante : peut être simplifiée.
    }
    else
    {
        return false;
    }
    // Mieux : return x > 0;
}
