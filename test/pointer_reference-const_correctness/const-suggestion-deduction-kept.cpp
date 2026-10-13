// SPDX-License-Identifier: Apache-2.0
// #217: rewriting a forwarding reference as a const lvalue reference changes what an lvalue
// argument deduces: F is const G& before the rewrite and G after it. That specialization, and the
// lambda instantiated inside it, change their names, but not their code: they are paired, and both
// suggestions are kept.

// strict-expectation-details: true

// at line 25, column 0
// [ !Info! ] ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'f' in function 'int apply_twice<from_temporaries()::$_0>(from_temporaries()::$_0&&)' is an rvalue reference and is never used to modify the referred object
// ↳ consider passing by value (<anonymous type> f) or const reference (const <anonymous type> &f)
// ↳ current type: <anonymous type> &&f
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// at line 25, column 0
// [ !Info! ] ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'f' in function 'int apply_twice<from_temporaries()::$_1>(from_temporaries()::$_1&&)' is an rvalue reference and is never used to modify the referred object
// ↳ consider passing by value (<anonymous type> f) or const reference (const <anonymous type> &f)
// ↳ current type: <anonymous type> &&f
// ↳ checked in the analyzed files only, with their compile commands: they compile with the suggested type, and their generated code matches, metadata included, once debug information, diagnostic metadata, descriptive names and renamed symbols are set aside

// The lvalue passes a const reference already: no suggestion for that specialization.
// not contains: in function 'int apply_twice<from_lvalue()

template <typename F>
int apply_twice(F&& f)
{
    const auto twice = [&](int x) { return f(f(x)); };
    return twice(1);
}

int from_temporaries()
{
    return apply_twice([](int x) { return x & 1; }) | apply_twice([](int x) { return x | 2; });
}

int from_lvalue()
{
    const auto g = [](int x) { return x ^ 4; };
    return apply_twice(g);
}
