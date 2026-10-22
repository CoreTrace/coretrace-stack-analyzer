// SPDX-License-Identifier: Apache-2.0
// #217: specializations of the rewritten template whose deduced type changes are paired only when
// the correspondence is one to one, and what they select is still compared.
// - call: g passes G& and h passes const G&; after the rewrite both deduce G, so two
//   specializations become one. Without a one-to-one correspondence, nothing shows that the
//   rewrite keeps or changes what they do: both suggestions are kept, not checked, and the reason
//   names the specializations. The lambdas are noexcept, so that all three specializations get
//   the same attributes: only the correspondence tells them apart.
// - pick: the lvalue's specialization is paired, and the condition of its `if constexpr` changes
//   with what F deduces: the suggestion is removed.

// strict-expectation-details: true
// not contains: in function 'int pick<

// at line 28, column 0
// [ !Info! ] ConstParameterNotModified.ReferenceRvaluePreferValue: parameter 'f' in function 'int call<from_temporary()::$_0>(from_temporary()::$_0&&)' is an rvalue reference and is only read, or passed where a reference to const is expected
// ↳ consider passing by const reference (const <anonymous type> &f)
// ↳ current type: <anonymous type> &&f
// ↳ the callee's body is not checked: it may still modify the referred object through a cast
// ↳ not checked with Clang: 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' and 'int call<from_lvalues()::$_0 const&>(from_lvalues()::$_0 const&)' both become 'int call<from_lvalues()::$_0>(from_lvalues()::$_0 const&)': their correspondence is not one to one

// at line 28, column 0
// [ !Info! ] ConstParameterNotModified.Reference: parameter 'f' in function 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' is only read, or passed where a reference to const is expected
// ↳ current type: <anonymous type> &f
// ↳ suggested type: const <anonymous type> &f
// ↳ the callee's body is not checked: it may still modify the referred object through a cast
// ↳ not checked with Clang: 'int call<from_lvalues()::$_0&>(from_lvalues()::$_0&)' and 'int call<from_lvalues()::$_0 const&>(from_lvalues()::$_0 const&)' both become 'int call<from_lvalues()::$_0>(from_lvalues()::$_0 const&)': their correspondence is not one to one
template <typename F> int call(F&& f)
{
    return f(1);
}

int from_temporary()
{
    return call([](int x) noexcept { return x & 1; });
}

int from_lvalues()
{
    auto g = [](int x) noexcept { return x | 2; };
    const auto& h = g;
    return call(g) | call(h);
}

template <typename T> struct is_reference
{
    static constexpr bool value = false;
};

template <typename T> struct is_reference<T&>
{
    static constexpr bool value = true;
};

template <typename F> int pick(F&& f)
{
    if constexpr (is_reference<F>::value)
        return f(4);
    else
        return f(8);
}

int pick_temporary()
{
    return pick([](int x) { return x & 3; });
}

int pick_lvalue()
{
    const auto g = [](int x) { return x ^ 5; };
    return pick(g);
}

// The Clang check of const suggestions is experimental: it runs only with this option.
// analyzer-arg: --experimental-const-suggestion-check
