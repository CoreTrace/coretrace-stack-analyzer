// SPDX-License-Identifier: Apache-2.0
// A 16-byte struct taken by value is one parameter in C++, but the x86-64 System V ABI passes it
// as two IR parameters (name.coerce0, name.coerce1). AArch64 keeps it as one ([2 x i64]).
// ConstParameterNotModified must report the same source parameters whatever the target.

struct Text
{
    const char* data;
    unsigned long size;
};

struct Match
{
    int* base;
    long k;
};

long use(long value);

// `p` is never written through: the only parameter that could be const.
long read_only_pointer(int* p, Text name)
{
    return use(*p + static_cast<long>(name.size));
}

// `match` is already const, `label` is a struct by value: nothing to report.
long const_reference(const Match& match, Text label, bool flag)
{
    return use(match.k + static_cast<long>(label.size) + flag);
}

// The same list as SizeMinusKWrites.cpp's emitIssue, in a lambda.
long in_lambda(int* p, const Match& found, Text text)
{
    auto emit = [&](const Match& match, Text sinkName, bool hasPtrDest)
    { return use(match.k + static_cast<long>(sinkName.size) + hasPtrDest + *p); };
    return emit(found, text, true);
}

struct Value
{
    int id;
};

struct Instruction
{
    int opcode;
};

long record(const Instruction* at, const Value* dest, const Value* base, bool pointer, long k);

// The parameter list of emitIssue in SizeMinusKWrites.cpp before #135, where alert #145 named
// `sinkName.coerce0`: sizeBase is the pointer that is never written through.
long old_emit_issue(Instruction* inst, Value* dest, Value* base, Text label)
{
    auto emitIssue =
        [&](Instruction* at, Value* to, Value* sizeBase, Text sinkName, bool hasPtrDest, long k)
    { return record(at, to, sizeBase, hasPtrDest, k + static_cast<long>(sinkName.size)); };
    return emitIssue(inst, dest, base, label, true, 1);
}
