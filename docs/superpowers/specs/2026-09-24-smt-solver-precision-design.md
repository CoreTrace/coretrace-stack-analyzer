# Précision du solveur SMT : Z3 en CI, interrogation systématique, conditions de chemin

Statut : design validé en conversation le 2026-09-24 ; référence pour le plan d'implémentation.
N'autorise pas la modification des tests existants : toute modification d'une fixture existante
reste soumise à un accord explicite.
Mesures de référence : issue #103. Reprend les étapes 1 et 2 de l'issue #80, fermée sans que
MemorySSA ait été ajouté.

## 1. Constat

La couche SMT (`--smt=on`) n'a aujourd'hui aucun effet observable. Mesures #103, Z3 contre SMT
désactivé :

| Corpus | Diagnostics | Requêtes | Diagnostics changés |
|---|---|---|---|
| Fixtures (309 fichiers) | 528 | 55, dont 3 `unsat` | 0 |
| Lua 5.4 (32 fichiers) | 310 | 78, toutes `sat` | 0 |
| zlib (8 fichiers exploitables sur 15) | 28 | 3, toutes `sat` | 0 |

Causes, vérifiées sur `main` (cc4edcb) :

1. Les requêtes ne contiennent que l'intervalle qui a déjà produit l'alerte
   ([StackBufferAnalysis.cpp:695](../../../src/analysis/StackBufferAnalysis.cpp#L695)). Aucune
   condition de branche n'est encodée ; seuls les `llvm.assume` du bloc courant le sont
   ([SmtEncoding.cpp:548](../../../src/analysis/smt/SmtEncoding.cpp#L548)).
2. Le code analysé est compilé en `-O0` ([ArgParser.cpp:1217](../../../src/cli/ArgParser.cpp#L1217)).
   Chaque lecture d'une variable locale est un `load` distinct, et l'encodeur fait de chaque `load`
   un symbole libre ([SmtEncoding.cpp:444](../../../src/analysis/smt/SmtEncoding.cpp#L444)). Le `i`
   d'une garde et le `i` de l'accès qu'elle protège sont donc deux inconnues sans lien.
3. Les règles n'interrogent le solveur que si une plage est connue (huit préconditions, §4.1).
   Sans plage, aucune requête : `return 1 + s->n;` (`unsigned char n`) et
   `memcpy(d, s, l * sizeof(char));` restent signalés, alors que ni `add nsw i32 1, zext i8` ni
   `mul i64 %l, 1` ne peuvent déborder.
4. `recursion` indexe ses plages par slot, c'est-à-dire par un pointeur
   ([StackComputation.cpp:374](../../../src/analysis/StackComputation.cpp#L374)), et l'encodeur
   ignore les valeurs non entières ([SmtEncoding.cpp:455](../../../src/analysis/smt/SmtEncoding.cpp#L455)).
   Ses 68 requêtes Lua sont toutes `sat`.
5. La CI ne compile pas Z3 : « SMT Z3 backend disabled (Z3 not found) » sur Ubuntu et macOS
   (run 35666706010). `--smt-backend=z3` répond alors `Unknown` sans avertissement, et la passe
   smt-z3 de `run_test.py` ne vérifie rien. Le paquet `libz3-dev` d'Ubuntu 24.04 ne fournit que
   `z3.pc`, pas `Z3Config.cmake`, que la détection CMake exige
   ([CMakeLists.txt:116](../../../CMakeLists.txt#L116)).
6. `IntervalBackend` ignore les assertions
   ([SolverOrchestrator.cpp:29](../../../src/analysis/smt/SolverOrchestrator.cpp#L29)).

L'information manquante est pourtant dans l'IR. Encodées à la main avec leur condition de chemin,
ces sondes sont réglées par Z3 :

| Sonde | Aujourd'hui, avec Z3 | Encodage avec chemin |
|---|---|---|
| garde CERT INT32-C, puis `return a + b;` | alerte, 0 requête | `unsat` |
| `if (n > SIZE_MAX / 8) return 0; return malloc(n * 8);` | alerte, 0 requête | `unsat` |
| `i ≤ 20`, `i < n`, `n ≤ 16`, puis `buf[i] = 1;` (`char buf[16]`) | alerte, 1 requête `sat` | `unsat` |

## 2. Objectif et périmètre

Objectif : **retirer des faux positifs sans jamais masquer un vrai positif.** Le solveur reste un
filtre : seule une réponse `unsat` agit, et elle supprime le diagnostic. `sat`, `unknown`,
`timeout` et `error` le conservent, comme aujourd'hui.

Contraintes :

- `--smt` reste désactivé par défaut. Sans `--smt=on`, aucun résultat ne change.
- Quand le SMT est inactif pour une règle, ses évaluateurs n'encodent aucune requête et ne
  construisent pas MemorySSA. Aujourd'hui, l'encodage a lieu avant ce test : il est l'argument de
  `evaluateQuery` ([SmtRefinement.hpp:59](../../../include/analysis/smt/SmtRefinement.hpp#L59)),
  qui ne vérifie qu'ensuite si le SMT est actif. Sans ce court-circuit, A1 et surtout A2
  alourdiraient l'analyse même SMT désactivé.
- Z3 est le backend de référence. Le backend par défaut (`interval`) ne change pas.
- Le coût reste borné par les réglages existants : 50 ms par requête (`--smt-timeout-ms`) et
  10 000 nœuds par requête (`--smt-budget-nodes`).

Trois étapes successives, chacune avec son issue et sa PR (§8) :

| Étape | Contenu | Gain attendu |
|---|---|---|
| A0 | Z3 réellement exécuté en CI | la passe smt-z3 devient significative |
| A1 | interrogation systématique, sémantique fidèle | faux positifs démontrables par la largeur de bits |
| A2 | conditions de chemin et modèle mémoire | faux positifs protégés par des gardes relationnelles ou disjonctives |

Hors périmètre :

- `recursion`. Pour cette règle, un `unsat` *ajoute* un diagnostic (récursion sans cas de base) :
  c'est un rôle de détecteur, à décider après mesure. Son encodage ne change pas.
- La détection de violations certaines et les contre-exemples dans les messages (même décision).
- Z3 dans l'image Docker et dans les binaires de release ; le changement du backend par défaut.
- Les étapes 3 et 4 de #80 : remplacer `singleStoredInteger`
  ([IntRanges.cpp:190](../../../src/analysis/IntRanges.cpp#L190)) et migrer les quatre copies de
  résolution de slots.
- `TypeConfusion` et les autres règles sans intégration SMT.
- Deux faux négatifs d'`integer-overflow` constatés pendant la conception : aucune alerte quand
  l'opérande vient d'un slot écrit plusieurs fois (`a = b; return a + 1;`) ou d'une variable dont
  l'adresse s'échappe (`escape(&x); return x + 1;`).

## 3. A0 — Z3 réellement exécuté en CI

### 3.1 Détection de Z3

Si `find_package(Z3 CONFIG)` ne trouve rien, la détection se replie sur pkg-config
(`pkg_check_modules(Z3 QUIET IMPORTED_TARGET z3)`) et lie la cible importée `PkgConfig::Z3`. Le message
`SMT Z3 backend enabled` reste celui qui atteste la détection. Un build sans Z3 se comporte comme
aujourd'hui : `ENABLE_Z3_BACKEND` vaut `ON` par défaut, et l'absence de Z3 n'arrête pas la
configuration.

### 3.2 CI

`ci.yml` installe `libz3-dev` et `pkg-config` sous Ubuntu, `z3` sous macOS. Le workflow Docker
(`test-ci-integration.yml`) n'exécute pas `run_test.py` et n'est pas concerné.

### 3.3 Avertissement de backend indisponible

Quand `--smt=on` est actif et qu'un backend utilisé par le mode choisi n'est pas compilé,
l'analyseur écrit une seule fois sur stderr :

```text
warning: SMT backend 'z3' is not available in this build; its queries are inconclusive
```

Le backend principal est toujours concerné. Le backend secondaire ne l'est que dans les modes
`cross-check`, `portfolio` et `dual-consensus`, les seuls qui l'utilisent. L'avertissement est
émis là où la configuration est validée, une fois par processus, et non dans chaque évaluateur. Le
module SMT expose pour cela la liste des backends compilés. Le fragment
`is not available in this build` est stable, car `run_test.py` s'en sert. Le code de retour, les
diagnostics et les sorties JSON et SARIF sur stdout ne changent pas.

### 3.4 Harness

Nouvelle vérification CLI `check_smt_unavailable_backend_warning` :

- `--smt=on --smt-backend=z3` ne doit produire aucun avertissement. Sans Z3, le run échoue donc
  une seule fois, avec un message qui demande d'installer Z3 et de reconstruire, au lieu de
  laisser la passe smt-z3 ne rien vérifier (ou d'échouer sur chaque fixture).
- `--smt=on --smt-backend=cvc5` (jamais compilé) produit l'avertissement, une seule fois ;
  `--smt=on --smt-backend=Interval` n'en produit pas, la casse étant ignorée.
- Le backend secondaire n'est signalé qu'en mode `portfolio` (pas en `single`), et
  `--smt-backend=cvc5 --smt=off` ne produit aucun avertissement.

### 3.5 Tests d'abord

La vérification de §3.4 est écrite en premier et échoue tant que §3.3 n'existe pas. Après A0, la
CI affiche `SMT Z3 backend enabled` sur les deux systèmes, et toute la suite reste verte.

## 4. A1 — Interrogation systématique, sémantique fidèle

### 4.1 Préconditions retirées

Préalable : l'encodage devient paresseux. `SmtConstraintEvaluator` n'appelle l'encodeur que si le
SMT est actif pour sa règle (§2). Sinon il répond `Inconclusive` sans rien construire.

Les huit conditions « plage connue requise » disparaissent ensuite :

| Fichier | Lignes |
|---|---|
| [IntegerOverflowAnalysis.cpp](../../../src/analysis/IntegerOverflowAnalysis.cpp) | 754, 766 et 782 (`shouldSuppressRiskWithSmt`) ; 837 (débordement signé) |
| [SizeMinusKWrites.cpp](../../../src/analysis/SizeMinusKWrites.cpp) | 495 |
| [OOBReadAnalysis.cpp](../../../src/analysis/OOBReadAnalysis.cpp) | 347 |
| [StackBufferAnalysis.cpp](../../../src/analysis/StackBufferAnalysis.cpp) | 691, 708 |

Une requête sans plage ne contient que la sémantique de l'IR. Ses symboles libres couvrent toutes
les valeurs possibles, donc un `unsat` reste une preuve. Ce qui est *transmis* au solveur ne change
pas : la politique d'IntegerOverflow qui ne passe que des plages bornées des deux côtés
([IntegerOverflowAnalysis.cpp:699](../../../src/analysis/IntegerOverflowAnalysis.cpp#L699)) est
conservée.

### 4.2 Sémantique modulaire

Les assertions `nsw` et `nuw` ajoutées par `encodeBinaryOperator`
([SmtEncoding.cpp:291-308](../../../src/analysis/smt/SmtEncoding.cpp#L291)) sont supprimées. Chaque
opération est encodée comme l'arithmétique modulaire que le code `-O0` exécute.

Justification : `reachesReturn`
([IntegerOverflowAnalysis.cpp:419](../../../src/analysis/IntegerOverflowAnalysis.cpp#L419)) ne suit
pas les opérations arithmétiques. Dans `return (a + 1) - 1;`, seul le `sub` est signalé. Avec
l'hypothèse `nsw` sur `a + 1`, Z3 répond `unsat` et supprimerait le seul signalement de ce
débordement. Sans elle, il répond `sat` avec `a = 0x7fffffff` (vérifié).

L'opération interrogée est déjà reconstruite sans ses drapeaux par
`encodeSignedOverflowFeasibility` et `encodeUnsignedOverflowFeasibility`. Ce comportement, protégé
par `test/integer-overflow/nsw-flag-must-not-discharge-itself.c`, ne change pas.

### 4.3 Attentes par passe dans `run_test.py`

Les deux formes d'attente acceptent un préfixe de passe placé juste après `//` :

```c
// [default] at line 2, column 42
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// [smt-z3] not contains: potential signed integer overflow in arithmetic operation
```

- Sans préfixe, une attente vaut pour les deux passes, comme aujourd'hui.
- Préfixes admis : `default` et `smt-z3`. Un préfixe inconnu fait échouer le fichier, pour qu'une
  faute de frappe ne désactive pas une attente en silence. Le préfixe n'est reconnu que sur la
  première ligne d'un bloc `at line` ou devant `not contains:` ; les lignes de suite d'un bloc
  (`// [ !!Warn ] …`) ne sont jamais interprétées comme un préfixe.
- Quand un fichier active le compte strict de diagnostics, ce compte s'évalue par passe, sur les
  attentes qui s'appliquent à cette passe.
- Ce changement ne modifie aucune fixture existante.

### 4.4 Tests d'abord

Nouvelles fixtures dans `test/integer-overflow/`, une fonction par fichier. Leur comportement dans
la passe default a été vérifié :

| Fixture | Code | default | smt-z3 |
|---|---|---|---|
| `smt-byte-operand-cannot-overflow.c` | `return 1 + s->n;` avec `unsigned char n` | alerte | absente |
| `smt-size-times-one-cannot-overflow.c` | `memcpy(d, s, l * sizeof(char));` | alerte | absente |
| `smt-chained-overflow-is-kept.c` | `return (a + 1) - 1;` | alerte | alerte |

Un test unitaire vérifie en outre qu'un évaluateur dont la règle n'a pas le SMT actif n'appelle
pas l'encodeur.

Ordre d'implémentation :

1. Écrire le test unitaire et les trois fixtures.
2. Rendre l'encodage paresseux (§4.1, préalable). Le test unitaire passe.
3. Retirer les préconditions (§4.1). Les deux premières fixtures passent ; la troisième échoue, à
   cause de l'hypothèse `nsw`.
4. Passer à la sémantique modulaire (§4.2). La troisième fixture repasse.

### 4.5 Effet attendu

Sur l'échantillon Lua, 8 faux positifs sont démontrables par la seule largeur de bits :

- 4 `x * sizeof(char)` : `lauxlib.c:594`, `lauxlib.c:613`, `lstring.c:238`, `lstring.c:257` ;
- 4 `1 + champ` sur des champs `lu_byte` ou `unsigned short` : `lgc.c:637`, `lgc.c:665`,
  `lgc.c:679`, `lparser.c:240`.

Coût : une requête par diagnostic candidat de ces quatre règles, 50 ms au plus chacune.

## 5. A2 — Conditions de chemin et modèle mémoire

### 5.1 MemorySSA dans `FunctionFacts`

[FunctionFacts](../../../include/analysis/FunctionFacts.hpp) gagne `AAResults` et `MemorySSA`,
construits à la première utilisation. `AAResults` utilise BasicAA, bâti sur les
`TargetLibraryInfo`, `AssumptionCache` et `DominatorTree` que `FunctionFacts` possède déjà.
`FunctionFacts` expose une seule requête :

```cpp
/// Access that may last have written the memory read by @p load (MemorySSA clobber).
[[nodiscard]] const llvm::MemoryAccess* clobberingAccess(const llvm::LoadInst& load) const;
```

`FunctionFacts` reste l'unique propriétaire de ces analyses. Les étapes 3 et 4 de #80 pourront
construire leur requête de définitions atteignantes sur ce même membre.

### 5.2 Encodage des lectures

Quand l'encodeur dispose d'un `FunctionFacts`, il traite ainsi une lecture entière simple `L`
(ni `volatile` ni atomique) du pointeur `p`, avec `C = clobberingAccess(L)` :

1. Si `C` est un `MemoryDef` dont l'instruction est un `store` simple vers `p` (à
   `stripPointerCasts` près) d'une valeur du même type que `L`, alors `L` est encodée par la valeur
   stockée.
2. Sinon, `L` est encodée par un symbole identifié par la paire `(p, C)`. Ce symbole est partagé
   par toutes les lectures de même paire.

Sans `FunctionFacts`, et pour toute autre lecture, le comportement actuel est conservé : un
symbole par `load`.

### 5.3 Condition d'atteignabilité

Pour une requête posée à l'instruction `I` du bloc `B` :

- **Graphe.** `G'` est le CFG privé des arcs retour donnés par `llvm::FindFunctionBackedges`. Si
  l'un de ces arcs a une cible qui ne domine pas sa source (cycle irréductible), aucune requête de
  la fonction ne reçoit de condition de chemin.
- **Région.** Pour un dominateur `D` de `B`, la région est l'ensemble des blocs de `G'`
  atteignables depuis `D` et depuis lesquels `B` est atteignable.
- **Formule.** `reach(D) = vrai`. Pour tout autre bloc `X` de la région,
  `reach(X) = ∨ (reach(P) ∧ cond(P → X))`, sur les prédécesseurs `P` de `X` dans la région.
  Chaque `reach(X)` est un nœud partagé du `ConstraintIR`.
- **Choix de `D`.** `D` est le dominateur de `B` le plus éloigné de `B` dont la requête complète
  tient dans `--smt-budget-nodes`. Si `B` est le bloc d'entrée, ou si même le dominateur immédiat
  dépasse le budget, la requête est celle de A1, sans condition de chemin.
- **`cond(P → X)`** :
  - `br i1 %c, T, F` : `c` vers `T`, `¬c` vers `F`, `vrai` si `T` et `F` sont le même bloc ;
  - `switch %v` : disjonction des `v = kᵢ` des cas qui mènent à `X` ; si `X` est la destination
    par défaut, on y ajoute, en disjonction, la conjonction des `v ≠ kᵢ` de tous les cas ;
  - tout autre terminateur, ou toute condition que l'encodeur ne sait pas traduire : `vrai`.
- **φ.** Un φ à plusieurs entrées reste un symbole frais, comme aujourd'hui.
- **Requête finale.** `reach(B) ∧ plages transmises par la règle ∧ assume du bloc ∧ violation`.

### 5.4 Pourquoi un `unsat` reste une preuve

Prenons une exécution réelle qui atteint `I`. Retirons, pour chaque boucle traversée, les
itérations antérieures à la dernière. On obtient un chemin de `D` à `B` sans arc retour, donc un
chemin de `G'`. Donnons à chaque symbole la valeur de sa dernière évaluation sur ce chemin :

- Une valeur SSA définie dans une boucle y est lue dans l'itération qui l'a définie.
- Les lectures qui partagent une paire `(p, C)` voient la même mémoire : aucune écriture n'a pu la
  modifier entre `C` et elles. Dans une boucle, `C` est le `MemoryPhi` d'en-tête : il représente
  la valeur à l'entrée de l'itération courante, pas la valeur initiale.

Cette affectation satisfait `reach(B)` et les conditions du chemin. Elle satisfait aussi la
violation si l'exécution la réalise. Un `unsat` exclut donc toute exécution réelle qui viole la
propriété en `I`.

Deux conditions sont nécessaires :

- La sémantique modulaire de §4.2. Avec l'hypothèse `nsw`, `if (a + 1 < a) return a + 2;`
  rendrait la branche impossible, alors qu'en `-O0` elle est prise pour `a = INT_MAX`.
- L'exclusion des régions irréductibles, pour lesquelles le découpage ci-dessus ne s'applique pas.

### 5.5 Intégration

Les évaluateurs des quatre règles de valeur (`integer-overflow`, `size-minus-k`, `stack-buffer`,
`oob-read`) reçoivent le `FunctionFacts` que chaque règle construit déjà, et le transmettent à
l'encodeur. Toutes leurs requêtes portent la condition de chemin de leur instruction de contexte.
`RecursionConstraintEvaluator` n'est pas modifié.

### 5.6 Tests d'abord

**Tests unitaires de l'encodeur**, dans `stack_usage_analyzer_unit_tests`, sur une entrée C dédiée
(`test/unit/smt_path_input.c`, chargée comme `int_range_point_input.c`). Ils inspectent le
`ConstraintIR` et ne demandent pas Z3 :

1. Deux lectures du même slot sans écriture entre elles donnent le même symbole.
2. Une lecture qui suit un `store` du même type au même slot est encodée par la valeur stockée.
3. Une écriture qui peut atteindre le slot, par un pointeur ou par un appel, sépare les symboles
   d'avant et d'après.
4. La condition d'une branche dominante figure dans la requête, avec la bonne polarité.
5. Un bloc à deux prédécesseurs produit une disjonction.
6. Une fonction à cycle irréductible ne reçoit pas de condition de chemin.
7. Une région qui dépasse le budget retombe sur la requête A1.

**Fixtures.** Leur comportement dans la passe default a été vérifié :

| Fixture | Code | default | smt-z3 |
|---|---|---|---|
| `test/integer-overflow/smt-path-cert-guarded-add.c` | garde CERT INT32-C, puis `return a + b;` | alerte | absente |
| `test/integer-overflow/smt-path-guarded-alloc-size.c` | `if (n > SIZE_MAX / 8) return 0; return malloc(n * 8);` | alerte | absente |
| `test/bound-storage/smt-path-relational-index-guard.c` | `i ≤ 20`, `i < n`, `n ≤ 16`, puis `buf[i] = 1;` | alerte | absente |
| `test/integer-overflow/smt-path-broken-overflow-check-is-kept.c` | `if (a + 1 < a) return a + 2;` | alerte | alerte |
| `test/bound-storage/smt-path-loop-counter-is-kept.c` | `int i = 0; while (i < 20) { buf[i] = 0; i++; }` | alerte | alerte |

Les deux dernières fixtures protègent la correction :

- la première exige la sémantique modulaire ;
- la seconde vérifie qu'une lecture dans la boucle n'est pas confondue avec la valeur initiale de
  l'indice.

Le cas « écriture entre la garde et l'usage » n'a pas de fixture de bout en bout. Aucune règle ne
signale aujourd'hui l'opération concernée : c'est l'un des faux négatifs listés au §2. Les tests
unitaires 2 et 3 portent cette propriété.

### 5.7 Documentation

La section « Current Implementation Status » de
[smt-solver-integration.md](../../architecture/smt-solver-integration.md) est mise à jour dans la PR
d'A2.

## 6. Compatibilité

- **Sans `--smt=on`** : aucun changement de résultat, de sortie ni de performance. C'est garanti
  par le court-circuit de §2.
- **Désactivation** : inchangée. `--smt=off` désactive tout, et `--smt-rules` restreint le SMT à
  certaines règles. Comme aujourd'hui, toute option `--smt-*` réactive le SMT : `--smt=off` doit
  donc venir après elles. `--print-effective-config` affiche l'état effectif.
- **Avec `--smt=on --smt-backend=z3`** : moins de diagnostics, puisque des faux positifs sont
  retirés. Les requêtes sont plus nombreuses (A1) et plus grandes (A2), dans les limites de temps
  et de budget existantes.
- **Nouvel avertissement** sur stderr quand un backend demandé n'est pas compilé. Les sorties JSON
  et SARIF sur stdout ne changent pas.
- **Build** : Z3 est détecté via pkg-config sous Debian et Ubuntu. Un build sans Z3 se comporte
  comme aujourd'hui.
- **`run_test.py`** : le run échoue désormais sans Z3, par une vérification unique. La syntaxe
  d'attentes existante ne change pas.

## 7. Validation (chaque PR)

```sh
cmake -S . -B build -DBUILD_ANALYZER_UNIT_TESTS=ON && cmake --build build -j 8
./build/stack_usage_analyzer_unit_tests .
./build/ownership_engine_unit_tests
python3 run_test.py --jobs 8
```

Ces commandes s'accompagnent d'un contrôle clang-format 20 sur les fichiers suivis modifiés.

**Mesure avant/après.** Elle reprend l'outil de #103 : un script local et la trace
`CTRACE_SMT_TRACE`, qui ne sont jamais commités. Elle tourne sur `main`, puis sur la branche, avec
deux configurations : SMT désactivé, et `--smt=on --smt-backend=z3` avec les cinq règles SMT.
Corpus : les fixtures, Lua 5.4 (32 fichiers), zlib (les 8 fichiers exploitables) et les sources du
projet (50 unités de `compile_commands.json`).

Le rapport de régressions de chaque PR donne :

- les diagnostics retirés et ajoutés, par règle, chacun classé vrai ou faux positif avec sa
  justification ;
- les requêtes par règle et par statut (`sat`, `unsat`, `unknown`, `timeout`, `error`) ;
- le temps total par corpus.

Critères d'acceptation :

- Aucun vrai positif retiré. Un vrai positif retiré bloque la PR.
- Toute la suite est verte, y compris la passe smt-z3 avec un vrai Z3 en CI.
- Avec le SMT désactivé, les diagnostics sont identiques à `main` et le temps d'analyse ne change
  pas au-delà du bruit de mesure.
- Toute fixture existante dont un diagnostic disparaît sous Z3 est listée. Son attente ne passe en
  `[default]` qu'avec un accord explicite.

## 8. Livraison

Trois issues et trois PR vers `main`, dans l'ordre A0, A1, A2. Chaque étape est utile seule : il
n'y a pas de branche d'intégration. Cette spec est livrée avec la PR d'A0. A1 et A2 partent de
`main` à jour.

À la création de l'issue d'A0, un commentaire sur #103 résume les mesures et renvoie aux trois
issues.

La présente modification porte uniquement sur la spec. La création des issues, des branches et des
PR, ainsi que l'implémentation, suivront le plan validé.
