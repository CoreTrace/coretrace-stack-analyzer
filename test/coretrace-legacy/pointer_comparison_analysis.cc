// pointer_cmp.c
#include <stdlib.h>
#include <stdio.h>

void bad_comparison(int* p, int* q)
{
    if (p < q)
    { // Comparaison entre pointeurs d'objets différents
        printf("p < q\n");
    }
    else
    {
        printf("p >= q\n");
    }
}

int main(void)
{
    int* p = (int*)malloc(sizeof(int)); // Première allocation
    int* q = (int*)malloc(sizeof(int)); // Deuxième allocation (objet différent)

    if (p == NULL || q == NULL)
        return 1; // Vérification basique

    *p = 10;
    *q = 20;

    if (p < q)
    { // Comparaison entre pointeurs d'objets différents
        printf("p < q\n");
    }
    else
    {
        printf("p >= q\n");
    }
    bad_comparison(p, q);
    free(p);
    free(q);
    return 0;
}
