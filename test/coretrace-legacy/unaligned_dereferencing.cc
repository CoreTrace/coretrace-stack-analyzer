// unaligned.c
#include <stdio.h>

int main(void)
{
    char buffer[8] = {0};       // Buffer de 8 octets
    char* ptr = buffer + 1;     // Pointe vers une adresse non alignée (offset de 1)
    int* unaligned = (int*)ptr; // Conversion en pointeur int (taille 4 octets)

    *unaligned = 42; // Déréférencement non aligné
    printf("Value: %d\n", *unaligned);
    return 0;
}
