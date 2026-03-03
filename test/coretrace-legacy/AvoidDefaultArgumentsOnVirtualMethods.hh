class Base
{
  public:
    virtual void func(int x = 10)
    {
        printf("Base: %d\n", x);
    } // Argument par défaut dans une méthode virtuelle.
};

class Derived : public Base
{
  public:
    void func(int x) override
    {
        printf("Derived: %d\n", x);
    }
};

int main(void)
{
    Base* b = new Derived();
    b->func(); // Comportement ambigu : l'argument par défaut de Base est utilisé.
}
