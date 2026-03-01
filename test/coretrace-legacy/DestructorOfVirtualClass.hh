class Base
{
  public:
    ~Base() {} // Destructeur non virtuel dans une classe de base avec des classes dérivées.
};

class Derived : public Base
{
  public:
    ~Derived()
    { /* Libération de ressources */
    }
};

int main()
{
    Base* obj = new Derived();
    delete obj; // Fuite mémoire : le destructeur de Derived n'est pas appelé.
}
