#ifndef TEST_FALSE_POSITIVE_REPRO_VENDOR_STB_IMAGE_H
#define TEST_FALSE_POSITIVE_REPRO_VENDOR_STB_IMAGE_H

static int stb_noise_read_uninitialized(int choose)
{
    int local;
    if (choose)
        local = 7;
    return local + 1;
}

#endif
