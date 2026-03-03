// init_with_error.c
#include <stdlib.h>

int* xxx()
{
    return (int*)malloc(sizeof(int)); // Allocation réussie ou NULL
}

int yyy(int* ptr)
{
    if (ptr == NULL)
        return -1; // Erreur si ptr est NULL
    *ptr = 42;     //Ascending;            // Déréférencement potentiel de NULL
    return 0;
}

int zzz(int* ptr)
{
    *ptr = 100; // Déréférencement potentiel de NULL
    return 0;
}

int init(void)
{
    int* ptr = xxx(); // Peut retourner NULL
    if (ptr == NULL)
    {
        return -1; // Erreur dans xxx
    }

    int status = yyy(ptr);
    if (status < 0)
    {
        free(ptr);
        return -2; // Erreur dans yyy
    }

    zzz(ptr);
    free(ptr);
    return 0; // Succès
}

int main(void)
{
    int result = init();
    if (result == 0)
    {
        return 1;
    }
    return 0;
}

// not working