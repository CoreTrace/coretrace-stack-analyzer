#include <stdio.h>
#include <string.h>

void vulnerable_function(char* input)
{
    char buffer[8]; // Un petit buffer de 8 octets
    printf("Adresse du buffer : %p\n", (void*)buffer);

    // Copie de l'input sans vérification -> peut provoquer un buffer overflow
    strcpy(buffer, input);

    printf("Contenu du buffer : %s\n", buffer);
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <input>\n", argv[0]);
        return 1;
    }

    vulnerable_function(argv[1]);

    printf("Programme terminé normalement !\n");
    return 0;
}
