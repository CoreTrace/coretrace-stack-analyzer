struct Base
{
    virtual ~Base() = default;
};

int main()
{
    Base b;
    return 0;
}

// not contains: parameter 'this' in function 'Base::~Base()'
