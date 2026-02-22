#include "mtu_api.h"

int mtu_worker(int x)
{
    char local_buf[16];
    local_buf[0] = (char)x;
    return (int)local_buf[0];
}
