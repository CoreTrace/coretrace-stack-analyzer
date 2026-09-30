// SPDX-License-Identifier: Apache-2.0
//
// Calls through declarations without a prototype (#157). With cross-tu-const-param-def.c, the call
// to read_only() passes as many arguments as its definition has parameters: p could point to
// const, as within one file. The call to read_pair() passes one argument for two parameters: the
// fact does not apply to it.

int read_only();
int read_pair();

int uses_noproto(int* p)
{
    return read_only(p);
}

int uses_noproto_short(int* p)
{
    return read_pair(p);
}
