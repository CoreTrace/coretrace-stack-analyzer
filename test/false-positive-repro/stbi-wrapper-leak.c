// SPDX-License-Identifier: Apache-2.0
typedef unsigned long size_t;

extern void* malloc(size_t size);
extern void free(void* ptr);

static unsigned char* stbi_load(const char* path, int* w, int* h, int* channels,
                                int desired_channels)
{
    (void)path;
    (void)w;
    (void)h;
    (void)channels;
    (void)desired_channels;
    return (unsigned char*)malloc(16u);
}

static void stbi_image_free(void* ptr)
{
    free(ptr);
}

// resource-model: models/resource-lifetime/generic.txt
int fp_stbi_wrapper_leak(void)
{
    int w = 0;
    int h = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load("dummy", &w, &h, &channels, 4);

    stbi_image_free(pixels);
    return 0;
}

// not contains: potential resource leak: 'HeapAlloc' acquired in handle 'pixels' is not released in this function
