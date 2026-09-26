// SPDX-License-Identifier: Apache-2.0
// A 16-byte struct taken by value is one parameter in C++, but the x86-64 System V ABI passes it
// as two IR parameters (label.coerce0, label.coerce1), after `this`. AArch64 keeps it as one.
// A method call marks its receiver as constructed, whatever the target.

struct Text
{
    const char* data;
    unsigned long size;
};

struct Widget
{
    int value;
    void touch(Text label);
};

void Widget::touch(Text label)
{
    (void)label;
}

// The call marks `w` as constructed: no "never initialized" report.
int call_touch(Text t)
{
    Widget w;
    w.touch(t);
    return 0;
}
