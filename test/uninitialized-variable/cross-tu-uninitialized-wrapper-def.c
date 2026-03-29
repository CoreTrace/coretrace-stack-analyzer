// SPDX-License-Identifier: Apache-2.0
typedef struct IntOutProps
{
    int* out;
} IntOutProps;

extern int ext_status_fill(int* out);

void fill_wrapper_cross_tu(const IntOutProps* props)
{
    if (ext_status_fill(props->out) != 0)
        return;
}
