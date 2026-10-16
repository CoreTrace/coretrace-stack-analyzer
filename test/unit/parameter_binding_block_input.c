// SPDX-License-Identifier: Apache-2.0
// A block's invoke function takes the block first, as `.block_descriptor`: an artificial
// parameter that, unlike vtt, the debug type of the function lists. Compiled with -fblocks.

int call_block(int (^block)(int*, double*), int* a, double* b)
{
    return block(a, b);
}

int use_block(int* a, double* b)
{
    return call_block(
        ^(int* x, double* y) {
          return *x + (int)*y;
        },
        a, b);
}
