#include <stdio.h>

/*
 * 1) Cas simples : borne sup OK / pas OK
 */

// AUCUN WARNING attendu (UB = 9, taille = 10)
void ub_ok(int i)
{
    char buf[10];

    if (i <= 9)
        buf[i] = 'A';
}

// WARNING UB attendu (UB = 10, taille = 10)
void ub_overflow(int i)
{
    char buf[10];

    // at line 27, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    if (i <= 10)
        buf[i] = 'B';
}

/*
 * 2) Borne inf négative : index potentiellement < 0
 */

// WARNING LB négatif attendu (i >= -3 && i < 5)
void lb_negative(int i)
{
    char buf[10];

    // at line 45, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -3 (index may be < 0)
    //     (this is a write access)
    if (i >= -3 && i < 5)
        buf[i] = 'C';
}

// WARNING LB négatif + UB hors borne (i >= -3 && i <= 15)
void lb_and_ub(int i)
{
    char buf[10];

    // at line 65, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 15 (array last valid index: 9)
    //     (this is a write access)

    // at line 65, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -3 (index may be < 0)
    //     (this is a write access)
    if (i >= -3 && i <= 15)
        buf[i] = 'D';
}

/*
 * 3) if imbriqués : affiner l’intervalle (LB & UB)
 *
 *   if (i <= 10) {
 *       if (i > 5)
 *           buf[i] = 'E';
 *   }
 *
 * Ici, on sait que 6 <= i <= 10
 * avec buf[8] → UB hors borne
 */

// ATTENDU : UB hors borne (taille 8,  i ∈ [6,10])
void nested_if_overflow(int i)
{
    char buf[8];

    // at line 94, column 20
    // [!!] potential stack buffer overflow on variable 'buf' (size 8)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 7)
    //     (this is a write access)
    if (i <= 10)
    {
        if (i > 5)
        {
            buf[i] = 'E';
        }
    }
}

// Variante “safe” pour comparaison (taille 16,  i ∈ [6,10]) → idéalement aucun warning
void nested_if_ok(int i)
{
    char buf[16];

    if (i <= 10)
    {
        if (i > 5)
        {
            buf[i] = 'F';
        }
    }
}

/*
 * 4) Boucles : patterns classiques de for
 */

// AUCUN WARNING attendu (0 <= i < 10, taille 10)
void loop_ok(void)
{
    char buf[10];

    for (int i = 0; i < 10; ++i)
        buf[i] = 'G';
}

// WARNING UB attendu (0 <= i <= 10, taille = 10)
void loop_ub_overflow(void)
{
    char buf[10];

    // at line 137, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 10 (array last valid index: 9)
    //     (this is a write access)
    for (int i = 0; i <= 10; ++i)
        buf[i] = 'H';
}

// WARNING LB négatif attendu (-3 <= i < 5, taille = 10)
void loop_lb_negative(void)
{
    char buf[10];

    for (int i = -3; i < 5; ++i)
        buf[i] = 'I';
}

/*
 * 5) Cas unreachable mais avec accès hors borne
 *    (tu as déjà ce genre de logique, mais ça teste qu’on garde bien l’info)
 */

// ATTENDU : warning overflow + [info] unreachable
void unreachable_example(void)
{
    int i = 1;
    char buf[10];

    // at line 168, column 17
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     constant index 11 is out of bounds (0..9)
    //     (this is a write access)
    //     [info] this access appears unreachable at runtime (condition is always false for this branch)
    if (i > 10)
    { // condition fausse à l’exécution
        buf[11] = 'J';
    }
}

/*
 * 6) Aliasing de pointeur + intervalle (LB & UB)
 */

// ATTENDU : UB + LB négatif (p = buf)
void alias_lb_ub(int i)
{
    char buf[10];
    char* p = buf;

    // at line 194, column 14
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf -> arraydecay -> p
    //     index variable may go up to 12 (array last valid index: 9)
    //     (this is a write access)

    // at line 194, column 14
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: p -> arraydecay -> buf
    //     inferred lower bound for index expression: -2 (index may be < 0)
    //     (this is a write access)
    if (i >= -2 && i <= 12)
        p[i] = 'K';
}

// ATTENDU : aucun warning (0 <= i < 10)
void alias_ok(int i)
{
    char buf[10];
    char* p = buf;

    if (i >= 0 && i < 10)
        p[i] = 'L';
}

/*
 * 7) Combinaison bizarre : bornes serrées, mais toujours safe
 *    i ∈ [2,7], buf[8] → normalement OK
 */

void tight_range_ok(int i)
{
    char buf[8];

    if (i >= 2 && i <= 7)
        buf[i] = 'M';
}

/*
 * 8) Cas extrême : bornes très larges
 *    i >= -100 && i <= 100, buf[10] → LB négatif + UB hors borne
 */

void huge_range(int i)
{
    char buf[10];

    // at line 241, column 16
    // [!!] potential stack buffer overflow on variable 'buf' (size 10)
    //     alias path: buf
    //     index variable may go up to 100 (array last valid index: 9)
    //     (this is a write access)

    // at line 241, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    //     alias path: buf
    //     inferred lower bound for index expression: -100 (index may be < 0)
    //     (this is a write access)
    if (i >= -100 && i <= 100)
        buf[i] = 'N';
}

/*
 * main : juste pour que le compilateur ne vire pas tout si optimisation
 */

int main(void)
{
    ub_ok(5);
    ub_overflow(10);

    lb_negative(-1);
    lb_and_ub(20);

    nested_if_overflow(8);
    nested_if_ok(8);

    loop_ok();
    loop_ub_overflow();
    loop_lb_negative();

    unreachable_example();

    alias_lb_ub(0);
    alias_ok(5);

    tight_range_ok(3);
    huge_range(0);

    return 0;
}
