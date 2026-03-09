Voici comment je vois l'évolution, ordonnée par ROI décroissant et dépendances techniques.

---

## Phase 1 — Encoder LLVM IR → ConstraintIR expressif
*Le verrou qui bloque tout le reste*

Aujourd'hui `encodeRangeConstraints` ne sait encoder que des intervalles. Pour que Z3 apporte une vraie valeur sur les autres analyses, il faut un encoder qui traduit les instructions LLVM en expressions symboliques.

**Ce qu'il faut couvrir :**

| LLVM IR | ConstraintIR |
|---|---|
| `add nsw i32 %a, %b` | `Add(sym_a, sym_b, 32)` + assertion overflow = poison |
| `icmp slt i32 %x, 42` | `Slt(sym_x, Const(42, 32))` |
| `br i1 %cond, label %then, label %else` | Edge constraint sur les deux branches |
| `phi [%v1, %bb1], [%v2, %bb2]` | Disjonction : `Or(And(from_bb1, eq(sym, v1)), And(from_bb2, eq(sym, v2)))` |
| `getelementptr [10 x i32], ptr %p, i64 0, i64 %idx` | `Add(base, Mul(idx, 4))` avec bound `0 ≤ idx < 10` |
| `select i1 %c, i32 %a, i32 %b` | `ITE(cond, sym_a, sym_b)` (nécessite ajout `ExprKind::Select`) |
| `llvm.assume(i1 %x)` | Assertion directe |
| `freeze` | Marquer le symbole comme "valeur définie" |

**Livrable :** un `LlvmConstraintEncoder` qui prend un chemin de blocs basiques et produit un `ConstraintIR` complet. Le `ConstraintIrBuilder` est déjà prêt — c'est la traversée LLVM qui manque.

---

## Phase 2 — Onboarding IntegerOverflow + SizeMinusK
*Le plus gros gisement de FP réductibles*

Avec l'encoder de la phase 1, on peut brancher le SMT sur les analyses à fort FP :

**IntegerOverflowAnalysis** — Aujourd'hui l'analyse détecte les overflows sur les arguments de `malloc`/`memcpy`. Le pattern type de FP :
```c
size_t n = get_count();    // range inconnu
void *p = malloc(n * sizeof(int));  // flaggé overflow
```
Avec Z3 : encoder `n * 4` en bitvector 64 bits, vérifier si l'overflow est satisfiable sous les contraintes de chemin. Si `UNSAT` → supprimer le diagnostic.

**SizeMinusKWrites** — Même principe : les off-by-one sont souvent des FP quand la taille est prouvablement > k.

**Point d'intégration :** après la passe heuristique existante, avant émission. Exactement ce que le doc prévoit. Le `RecursionConstraintEvaluator` sert de modèle — créer un `OverflowConstraintEvaluator` analogue.

---

## Phase 3 — StackBuffer + OOBRead path-sensitive
*Précision sur les accès mémoire*

L'analyse de buffer overflow actuelle est flow-insensitive — elle ne suit pas les contraintes de chemin sur les indices. Avec le SMT :

```c
int buf[10];
int idx = get_index();
if (idx >= 0 && idx < 10) {
    buf[idx] = 42;  // aujourd'hui : flaggé potentiel overflow
                     // avec SMT : UNSAT → sûr
}
```

C'est le même pattern que la récursion : BFS avec accumulation de contraintes, sauf que la question est "cet accès est-il dans les bornes ?" au lieu de "cette base case est-elle atteignable ?".

**Possibilité de factoriser** la traversée path-sensitive de `hasFeasibleNonRecursiveReturnPath` en un framework réutilisable :

```
PathExplorer<Question>
  - traverse le CFG en BFS
  - accumule les contraintes
  - pose une Question à chaque point d'intérêt
  - retourne Feasible / Infeasible / Inconclusive
```

---

## Phase 4 — TypeConfusion avec raisonnement struct-layout
*Les 91 FPs documentés*

C'est le plus gros lot de FP en volume mais le plus complexe à encoder. Il faut :

1. Encoder les layouts de structs (offsets des champs, tailles) comme contraintes
2. Pour chaque "conflit de type" détecté, vérifier si les deux vues sont sur des sous-objets légalement imbriqués
3. Si Z3 prouve que `offset_A + size_A ≤ offset_B` ou que les deux accès sont au même sous-objet → `UNSAT` → supprimer

Ça nécessite probablement un ajout au `ConstraintIR` : des contraintes de type mémoire/layout, pas seulement arithmétiques.

---

## Phase 5 — DiagnosticRefiner comme composant autonome
*Quand plusieurs analyses utilisent le SMT*

Dès que 3+ analyses passent par le SMT, la logique de décision (keep/suppress/downgrade/tag inconclusive) mérite d'être extraite :

```cpp
class DiagnosticRefiner {
    RefinementDecision refine(const Diagnostic& original,
                              const SmtDecision& decision,
                              const RulePolicy& policy) const;
};

enum class RefinementDecision {
    Keep,           // SAT ou pas de SMT
    Suppress,       // UNSAT confirmé
    Downgrade,      // UNSAT mais confidence < seuil → Warning→Info
    MarkInconclusive // Unknown/Timeout
};
```

Avec des politiques par rule :
- `recursion` : suppress agressif (UNSAT = pas de bug)
- `integer-overflow` : downgrade plutôt que suppress (risque d'encoding bug)
- `type-confusion` : suppress seulement en dual-consensus

---

## Phase 6 — Cache + Observabilité
*Quand le volume de queries justifie l'investissement*

**Cache :**
- Clé : hash du `ConstraintIR` normalisé + mode/backend
- Valeur : `SmtStatus`
- Portée : process-local (un seul run d'analyse)
- ROI : significatif en mode portfolio/cross-check ou quand plusieurs analyses posent des questions similaires sur les mêmes fonctions

**Observabilité :**
- Compteurs : queries total / sat / unsat / unknown / timeout / error
- Latence : P50/P95/P99 par backend
- Suppressions : count par rule
- Format : JSON ou intégration dans le `--timing` existant

---

## Phase 7 — Améliorations Z3 avancées
*Optimisations pour passer à l'échelle*

- **Incremental solving** : réutiliser le `z3::solver` avec `push/pop` au lieu de recréer un contexte par query. Gros gain sur les traversées de chemin où les queries partagent un préfixe commun
- **Tactics** : utiliser `z3::tactic("simplify") & z3::tactic("solve-eqs") & z3::tactic("bit-blast") & z3::tactic("sat")` pour les formules bitvector plutôt que le solveur par défaut
- **Parallel portfolio réel** : `std::async` pour lancer Z3 et cvc5 en parallèle dans le mode portfolio (aujourd'hui c'est séquentiel)

---

## Résumé visuel

```
Phase 1: Encoder LLVM→ConstraintIR    ← verrou technique
Phase 2: IntegerOverflow + SizeMinusK  ← plus gros ROI en FP
Phase 3: StackBuffer + OOBRead         ← path-sensitive generalisé
Phase 4: TypeConfusion                 ← 91 FPs mais complexe
Phase 5: DiagnosticRefiner autonome    ← quand 3+ rules utilisent SMT
Phase 6: Cache + Observabilité         ← quand le volume le justifie
Phase 7: Incrémental + tactics + //    ← scaling
```

Les phases 1-2 sont les plus urgentes. Les phases 3-4 dépendent de la qualité de l'encoder. Les phases 5-7 sont de l'optimisation.
