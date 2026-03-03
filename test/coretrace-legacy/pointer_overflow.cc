#include <stdlib.h>

int main(void)
{
    int* array = (int*)malloc(5 * sizeof(int));
    if (array == NULL)
        return 1;
    int* ptr = array;
    for (int i = 0; i < 10; i++)
    {
        ptr = ptr + 1; // Déborde après i = 5
        *ptr = i;
    }
    free(array);
    return 0;
}
