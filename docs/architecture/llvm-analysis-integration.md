<!-- SPDX-License-Identifier: Apache-2.0 -->
# Intégration des analyses LLVM amont

## Ce qui a été ajouté

`analysis::FunctionFacts` ([FunctionFacts.hpp](../../include/analysis/FunctionFacts.hpp)) est le
propriétaire unique, par fonction, des analyses LLVM amont : `AssumptionCache`, `DominatorTree`,
`TargetLibraryInfo`, `LazyValueInfo`, plus `ObjectSizeOffsetVisitor` et `computeConstantRange`
(ValueTracking / KnownBits) exposés derrière deux requêtes :

| Requête | Analyses utilisées |
|---|---|
| `signedRangeAt(v, at)` | LazyValueInfo (gardes dominant `at`, `llvm.assume`), ValueTracking/KnownBits, `!range` |
| `signedRange(v)` | idem, interrogé à la définition de `v` — donc valide partout où `v` est vivant |
| `objectSizeBytes(p)` | ObjectSizeOffsetVisitor + TargetLibraryInfo |

`computeIntRanges(F, facts)` remplace `computeIntRangesFromICmps(F)` chez les 5 consommateurs
(OOBRead, IntegerOverflow, AllocaUsage, StackBuffer, SizeMinusKWrites). Il part du scan d'`ICmp`
existant et **intersecte** chaque borne avec ce que `facts` prouve — l'intersection ne fait que
resserrer, donc un consommateur qui s'en sert pour écarter une alerte ne peut qu'en écarter
davantage, jamais moins.

`SizeMinusKWrites` construisait son propre `AssumptionCache` + `LazyValueInfo` + `TargetLibraryInfo` ;
il consomme désormais ceux de `FunctionFacts`.

## Le fait décisif : l'analyseur lit de l'IR `-O0`

[InputPipeline.cpp:176](../../src/analysis/InputPipeline.cpp#L176) force `-O0`, et le seul pass
exécuté est `function-attrs`. Chaque variable locale vit donc dans une `alloca` et chaque valeur
consommée est un `load`. LazyValueInfo, KnownBits et ScalarEvolution ne voient rien à travers la
mémoire — c'est exactement pourquoi le dépôt avait dû écrire son propre scan.

Deux conséquences, toutes deux mesurées et non supposées :

1. **`publishSingleStoreSlots`** ([IntRanges.cpp](../../src/analysis/IntRanges.cpp)) fait le pont :
   pour une `alloca` entière écrite une seule fois, jamais adressée, dont le store domine le load,
   la plage de la valeur stockée est publiée sous la slot **et** sous ses loads. Sans ce pont,
   toute l'intégration est inerte. C'est la contrepartie entière du *pointer-slot peeling* que les
   analyses de buffer font déjà.
2. **ScalarEvolution et LoopInfo ont été retirés.** Un compteur de boucle en `-O0` vit dans une slot
   à écritures multiples, et l'arithmétique que SCEV bornerait porte `nsw` — que l'on refuse
   volontairement (voir ci-dessous). Vérifié : SCEV ne change **aucun** résultat sur les
   217 fixtures ni sur un cas construit exprès. À ne réintroduire qu'avec un pass de promotion.

## Régressions rencontrées et corrigées

La première intégration naïve cassait **12 checks sur 1671**, en deux familles. Les deux étaient
de vraies fautes de ma part, pas des expectations mal calibrées.

### 1. Raisonnement circulaire sur les drapeaux `nsw`

`test/alloca/recursive-controlled-alloca.c`, `test/pointer_reference-const_correctness/const-mixed.c`,
`test/uninitialized-variable/uninitialized-local-cpp-lambda-receiver.cpp` — passe `smt-z3`.

`IntegerOverflowAnalysis` cherche à valider le drapeau `nsw` d'un `add`. Or LLVM **dérive** la plage
d'un `add nsw` de la promesse qu'il ne déborde pas. Publier cette plage revient à donner au
solveur l'hypothèse qu'il devait vérifier : Z3 concluait « débordement infaisable » et les trois
alertes disparaissaient.

Correctif : `restsOnWrapAssumption()` exclut de la map toute valeur définie par un
`OverflowingBinaryOperator` portant `nsw`/`nuw`.

> Limite connue : la circularité transitive (`%t = add nsw …` puis `%u = add nsw %t, 1`) n'est pas
> traitée. Elle n'existait pas avant puisque LVI n'était pas utilisé du tout.

### 2. Une borne qui ne borne rien

`test/vla/vla-read.c`, `test/vla/vla-scanf.c`, `test/vla/vla-unknown-stack.c` — passes `default` et `smt-z3`.

Pour `char buf[n]` avec `int n`, la taille est un `zext i32 %n to i64`. KnownBits en déduit
`[0, 4294967295]` — vrai pour tout `n`, et donc sans information. `getAllocaUpperBoundBytes`
acceptait toute borne supérieure, et le diagnostic basculait de

```
[ !!Warn ] user-controlled alloca size for variable 'vla'
    ↳ size is unbounded at compile time
    ↳ size depends on user-controlled input
```
vers un
```
[!!!Error] large alloca on the stack for variable 'vla'
    ↳ inferred upper bound for size: 4294967295 bytes
```
— strictement moins actionnable, et une « borne » de 4 Gio n'en est pas une.

Correctif : `trivialRange()` / `informativeBounds()` écartent toute borne qui ne fait que
redire ce que le type donne déjà, en regardant à travers `zext`/`sext`.

## État final

```
✅ Passed 1671/1671 tests.
```

Aucune régression. Diff comportemental sur les 248 fixtures, une seule différence :

| Fixture | Avant | Après |
|---|---|---|
| `test/alloca/wrong-alloca.c` | `↳ size is unbounded at compile time` | `↳ inferred upper bound for size: 261120 bytes` |

`uint8_t small_size * 1024` ⇒ au plus `255 * 1024 = 261120`. La nouvelle borne est exacte ;
l'ancienne sortie était un faux « non borné ». **Le bloc d'attente en commentaire de cette fixture
est désormais périmé alors que le test passe toujours** — voir la section couverture.

Temps d'exécution inchangé (dominé par le frontend clang) : 1,07 s → 1,01 s sur 40 fixtures.

---

# Couverture de tests

Aucune fixture existante n'a été modifiée. La suite passe **1711/1711** (contre 1671 auparavant,
où le binaire de tests unitaires n'était pas construit et la vérification était sautée).

## Le trou dans le harnais

`evaluate_pass` compare une attente en trois temps : sous-chaîne exacte, puis avec tolérance sur
la ligne/colonne, puis — en dernier recours — **en ne retenant que les lignes d'en-tête**. Ce
troisième niveau jette toutes les lignes `↳`. C'est ce qui laissait `test/alloca/wrong-alloca.c`
passer alors que son détail était devenu faux.

Plutôt que de durcir la comparaison pour tout le monde (ce qui aurait fait échouer des fixtures
existantes dont les détails ont dérivé depuis longtemps), le niveau 3 est désormais désactivable
par fichier :

```c
// strict-expectation-details: true
```

Le défaut est inchangé. Les nouvelles fixtures qui figent une valeur calculée l'activent.

## Ce qui est ajouté

| Test | Garde | Mécanisme |
|---|---|---|
| `integer-overflow/nsw-flag-must-not-discharge-itself.c` | circularité `nsw` | boîte noire, passe smt-z3 |
| `integer-overflow/knownbits-bounded-operand.c` | publication des plages, bout en bout | boîte noire |
| `vla/vla-zext-width-is-not-a-bound.c` | `trivialRange` | boîte noire + `not contains` |
| `alloca/knownbits-narrow-alloca-bound.c` | borne exacte `255 * 512 = 130560` | détails stricts |
| `security/oob-read/heap-index-masked.c` | `and` → KnownBits | boîte noire |
| `security/oob-read/heap-index-modulo.c` | `urem` → KnownBits | boîte noire |
| `security/oob-read/strlen-global-object-size.c` | `ObjectSizeOffsetVisitor` | détails stricts |
| `security/oob-read/heap-index-builtin-assume-limitation.c` | limitation `llvm.assume` documentée | détails stricts |
| `unit/analyzer_module_unit_tests.cpp::testIntRangeFacts` | les 4 invariants de la map | boîte blanche, 13 assertions |

Les tests unitaires ne sont pas construits par défaut :
```zsh
cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON && cmake --build build
```

## Pourquoi un test unitaire, et pas seulement des fixtures

Le refus de publier une plage pour une slot écrite plusieurs fois est l'invariant de solidité de
`publishSingleStoreSlots`. **Il n'est observable par aucun diagnostic** : le détecteur de
débordement d'entier comme celui d'accès hors bornes abandonnent devant une slot réécrite
*avant* de consulter la map — `dependsOnFunctionArgument` ne traverse pas une slot à écritures
multiples. Un pont incorrect ne produirait donc aucun échec de bout en bout. C'est exactement le
cas pour lequel `test/unit/` existe.

## Validation par mutation

Chaque garde a été cassée volontairement pour vérifier que les tests mordent :

| Mutation | Ce qui devient rouge |
|---|---|
| retirer `restsOnWrapAssumption` + `informativeBounds` | fixture `nsw` (smt-z3), `vla-zext` (2 passes), les 2 fixtures OOB, 2 assertions unitaires |
| accepter les slots à écritures multiples | **2 assertions unitaires uniquement** — aucune fixture |
| retirer `publishSingleStoreSlots` | les 2 fixtures OOB (2 passes), 2 assertions unitaires |
| retirer les deux voies de publication | `knownbits-bounded-operand.c` |

`restsOnWrapAssumption` seul ne suffit plus à reproduire la régression d'origine : depuis l'ajout
de `informativeBounds`, la borne supérieure issue du `nsw` est déjà écartée comme non
informative. Les deux gardes se recouvrent partiellement, d'où la mutation combinée.

## Correction d'une réserve du rapport précédent

La circularité `nsw` **transitive** que j'avais signalée est en fait couverte : dans une chaîne
`%t = add nsw …` puis `%u = add nsw %t, 1`, les deux maillons portent le drapeau et sont donc
tous deux exclus. Vérifié : le comportement est identique à la référence sur cette forme, dans
les deux passes. `testIntRangeFacts` contient la chaîne et l'assertion correspondante.

## Limitation confirmée : `llvm.assume` est inopérant

`FunctionFacts` possède un `AssumptionCache`, mais en `-O0` l'hypothèse contraint le `load` SSA
auquel `__builtin_assume` s'applique, tandis que l'accès indexé relit la slot par un *second*
`load` distinct. Les hypothèses ne se transmettent pas entre deux `load` d'une même `alloca`.
Mesuré : aucune différence avec la référence. La fixture correspondante fige ce comportement et
porte en commentaire l'instruction de l'inverser le jour où la propagation à travers les slots
existera.
