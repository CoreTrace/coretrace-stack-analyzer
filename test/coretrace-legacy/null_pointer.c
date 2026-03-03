// null_pointer_subtle.c
#include <stdlib.h>

int toto(void)
{
    int* ptr = NULL; // Pointeur initialisé à NULL
    *ptr = 42;       // Déréférencement de pointeur nul

    return 0;
}

int main(void)
{
    int* ptr = (int*)malloc(sizeof(int)); // Allocation qui peut échouer

    if (ptr != NULL)
    {
        *ptr = 10; // Accès valide si l'allocation réussit
    }
    *ptr = 42; // Déréférencement potentiel de NULL si malloc échoue
    free(ptr);
    toto();
    return 0;
}

// at line 7, column 10
// [!!!Error] potential null pointer dereference on '<pointer>'
