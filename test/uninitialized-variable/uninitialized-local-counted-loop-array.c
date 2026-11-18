// SPDX-License-Identifier: Apache-2.0
// #224: a counted loop that writes every element of an array initializes it, while a loop that
// leaves the element read unwritten does not.

int read_after_full_loop(void)
{
    int squares[4];
    for (int i = 0; i < 4; i++)
        squares[i] = i * i;
    return squares[3];
}

int read_after_decreasing_loop(void)
{
    int down[4];
    for (int i = 3; i >= 0; i--)
        down[i] = i;
    return down[0];
}

int read_after_do_while(void)
{
    int counted[4];
    int i = 0;
    do
    {
        counted[i] = i;
        i++;
    } while (i < 4);
    return counted[3];
}

static int ends_match(const int* values)
{
    return values[0] == values[3];
}

int pass_after_full_loop(void)
{
    int filled[4];
    for (int i = 0; i < 4; i++)
        filled[i] = i;
    return ends_match(filled);
}

int read_past_loop_end(void)
{
    int short_end[4];
    for (int i = 0; i < 3; i++)
        short_end[i] = i;
    return short_end[3];
}

int read_before_loop_start(void)
{
    int late_start[4];
    for (int i = 1; i < 4; i++)
        late_start[i] = i;
    return late_start[0];
}

int read_skipped_element(void)
{
    int every_other[4];
    for (int i = 0; i < 4; i += 2)
        every_other[i] = i;
    return every_other[1];
}

int read_after_conditional_write(int c)
{
    int some[4];
    for (int i = 0; i < 4; i++)
    {
        if (c > i)
            some[i] = i;
    }
    return some[3];
}

int read_after_loop_with_break(int c)
{
    int cut[4];
    for (int i = 0; i < 4; i++)
    {
        if (c == i)
            break;
        cut[i] = i;
    }
    return cut[3];
}

int read_after_variable_bound(int n)
{
    int bounded[4];
    for (int i = 0; i < n; i++)
        bounded[i] = i;
    return bounded[3];
}

// at line 51, column 12
// [ !!Warn ] potential read of uninitialized local variable 'short_end'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 59, column 12
// [ !!Warn ] potential read of uninitialized local variable 'late_start'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 67, column 12
// [ !!Warn ] potential read of uninitialized local variable 'every_other'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 78, column 12
// [ !!Warn ] potential read of uninitialized local variable 'some'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 90, column 12
// [ !!Warn ] potential read of uninitialized local variable 'cut'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 98, column 12
// [ !!Warn ] potential read of uninitialized local variable 'bounded'
// ↳ this load may execute before any definite initialization on all control-flow paths

// not contains: potential read of uninitialized local variable 'squares'
// not contains: potential read of uninitialized local variable 'down'
// not contains: potential read of uninitialized local variable 'counted'
// not contains: potential read of uninitialized local variable 'filled'
