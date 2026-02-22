#include "vendor/stb_image.h"

int fp_stb_vendor_header_filter(void)
{
    return stb_noise_read_uninitialized(0);
}

// not contains: potential read of uninitialized local variable 'local'
