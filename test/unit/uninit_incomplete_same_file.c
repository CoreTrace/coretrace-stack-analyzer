// SPDX-License-Identifier: Apache-2.0
// Input for testUninitializedIncompleteSummariesInOneFile (#168). The blocks of produce and
// consume are laid out before their predecessors, so each dataflow iteration carries the entry
// state one block further: a budget of two iterations stops them before their fixpoint.
// Their callers have a single block and converge within it.
void produce_decl(int* p, int n);
int consume_decl(int* p, int n);

// Writes *p on every path.
void produce(int* p, int n)
{
    goto L1;
L5:
    if (n > 0)
        *p = n;
    else
        *p = 0;
    return;
L4:
    goto L5;
L3:
    goto L4;
L2:
    goto L3;
L1:
    goto L2;
}

// Reads *p.
int consume(int* p, int n)
{
    goto L1;
L5:
    return *p + n;
L4:
    goto L5;
L3:
    goto L4;
L2:
    goto L3;
L1:
    goto L2;
}

int write_then_read(int n)
{
    int x;
    produce(&x, n);
    return x;
}

int write_then_read_decl(int n)
{
    int x;
    produce_decl(&x, n);
    return x;
}

int pass_to_reader(int n)
{
    int x;
    return consume(&x, n);
}

int pass_to_reader_decl(int n)
{
    int x;
    return consume_decl(&x, n);
}
