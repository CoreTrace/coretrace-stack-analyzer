// double_free.c
#include <stdlib.h>

int main(void)
{
    char* ptr = (char*)malloc(10); // Allocation de 10 octets
    if (ptr == NULL)
        return 1; // Vérification basique

    free(ptr); // Première libération
    free(ptr); // Deuxième libération (double free)

    return 0;
}
