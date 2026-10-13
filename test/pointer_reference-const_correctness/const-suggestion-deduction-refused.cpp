// SPDX-License-Identifier: Apache-2.0
// #217: specializations of the rewritten template whose deduced type changes are paired only when
// the correspondence is unique, and their code is still compared.
// - call: g passes G& and h passes const G&; after the rewrite both deduce G, so two
//   specializations become one. No unique correspondence: both suggestions are removed.
// - pick: the lvalue's specialization is paired, but its code changes with what F deduces.

// not contains: in function 'int call<
// not contains: in function 'int pick<

template <typename F>
int call(F&& f)
{
    return f(1);
}

int from_temporary()
{
    return call([](int x) { return x & 1; });
}

int from_lvalues()
{
    auto g = [](int x) { return x | 2; };
    const auto& h = g;
    return call(g) | call(h);
}

template <typename T>
struct is_reference
{
    static constexpr bool value = false;
};

template <typename T>
struct is_reference<T&>
{
    static constexpr bool value = true;
};

template <typename F>
int pick(F&& f)
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
