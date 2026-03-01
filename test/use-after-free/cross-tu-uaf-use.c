#include <stddef.h>
#include <string.h>

void* acquire_handle_wrapper(void);
void release_handle_wrapper(void* h);

/* inter-TU UAF dans un nested-if */
void io_cross_uaf_nested_if(int gate1, int gate2)
{
    void* h = acquire_handle_wrapper();
    if (!h)
        return;

    if (gate1)
    {
        if (gate2)
        {
            release_handle_wrapper(h);
        }
    }

    if (gate1 && gate2)
    {
        memset(h, 0, 1);
    }

    if (!(gate1 && gate2))
    {
        release_handle_wrapper(h);
    }
}

/* inter-TU double release dans un nested-loop */
void io_cross_double_release_nested_loop(int n)
{
    void* h = acquire_handle_wrapper();
    if (!h)
        return;

    for (int i = 0; i < n; ++i)
    {
        for (int j = 0; j < n; ++j)
        {
            if (i == 0 && j == 0)
            {
                release_handle_wrapper(h);
            }
        }
    }

    release_handle_wrapper(h);
}

int main(void)
{
    io_cross_uaf_nested_if(1, 1);
    io_cross_double_release_nested_loop(1);
    return 0;
}
