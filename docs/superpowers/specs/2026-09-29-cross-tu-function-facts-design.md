# Faits de fonction entre unités de compilation (#157, premier tableau)

Statut : proposition corrigée après une première relecture, qui a retenu A1 et B1 (§10) ; à valider
avant le plan d'implémentation. N'autorise ni le code ni la modification des tests : chaque PR
présentera ses changements de tests, avec leurs preuves, avant de les faire.
Issue : #157, premier tableau. Liens : #153 et #158 (fonctions sans retour entre fichiers), #159
(pile maximale des cycles d'appels), #156.
Mesures et références au code : `main` 071aa37.

## 1. Constat

Six règles suivent un appel jusque dans le corps de l'appelé quand il est défini dans le même
fichier. Elles le perdent quand il est défini dans un autre fichier de la même exécution. Chaque
cas a été analysé seul, puis réparti en `lib.c main.c` :

| Règle | Cas, appelé dans `lib.c` | Un fichier | Deux fichiers |
|---|---|---|---|
| `UninitializedLocalRead` | `int v; peek(&v); return v;`, où `void peek(int *p) { (void)p; }` | signalé | non signalé |
| `ConstParameterNotModified.Pointer` | `p` seulement passé à `read_only(const int *p)` | signalé | non signalé |
| `DuplicateIfCondition` | `if (is_ready(x)) … else if (is_ready(x))`, `is_ready` déterministe | signalé | non signalé |
| `SizeMinusOneWrite` | `my_copy(dst, src, n - 1)`, où `my_copy` passe `n` à `strncpy` | signalé | non signalé |
| `Recursion.Detected` | `rec_a` (`lib.c`) et `rec_b` (`main.c`) s'appellent | signalé | non signalé |
| Pile maximale | `big_caller` appelle `big_frame`, qui a un cadre de 4096 octets | 4128 octets | inconnue (≥ 16) |

Pour `UninitializedLocalRead`, l'appelé doit retourner `void`. Face à une déclaration, la règle
présume qu'une telle fonction écrit par ses arguments pointeurs (§5.1). Avec `int peek(int*)`, la
lecture est signalée dans les deux configurations.

## 2. Objectif et périmètre

Pour ces six règles, un appel vers une fonction définie dans un autre fichier de la même exécution
doit donner la même conclusion qu'un appel vers une fonction du même fichier. Aucun faux positif
nouveau, et le résultat d'une analyse sur un seul fichier ne change pas.

Quatre PR, de la plus petite à la plus lourde :

1. `UninitializedLocalRead` : le fait « n'écrit rien » traverse les fichiers (§5).
2. `ConstParameterNotModified` : le type des paramètres de la définition (§6).
3. `DuplicateIfCondition` et `SizeMinusOneWrite` : deux faits transitifs (§7).
4. Récursion et pile maximale : un graphe d'appels qui couvre tous les fichiers (§8).

Hors périmètre :

- le second tableau de #157 : les règles qui ne suivent pas les appels, même dans un seul fichier ;
- l'édition de liens des modules, ou l'analyse d'un module fusionné ;
- la persistance des nouveaux faits sur disque ;
- de nouvelles options de ligne de commande.

## 3. Existant

- **Chargement.** Plusieurs fichiers sont chargés ensemble par `analyzeWithSharedModuleLoading`. Chaque
  module a son `LLVMContext`, et tous restent chargés jusqu'à la fin. Les fichiers sont triés par nom
  avant le chargement ([AnalyzerApp.cpp:2509](../../../src/app/AnalyzerApp.cpp#L2509)).
- **Canaux entre fichiers.** Avant l'analyse des modules, quatre canaux sont calculés et passés par
  `AnalysisConfig` :
  - **Fonctions qui ne retournent jamais (#158).** Un ensemble de noms, agrandi par des passes sur
    tous les modules jusqu'à ce qu'il ne grandisse plus
    ([AnalyzerApp.cpp:969](../../../src/app/AnalyzerApp.cpp#L969)). Seules les définitions exactes
    à liaison externe y entrent ([ModulePasses.cpp:95](../../../src/passes/ModulePasses.cpp#L95)).
  - **Résumés de ressources et de lectures non initialisées.** `runCrossTUSummaryPass`
    ([CrossTUSummaryDriver.hpp:217](../../../src/app/CrossTUSummaryDriver.hpp#L217)) ordonne les
    modules selon leurs appels mutuels, appelés d'abord. Il itère 12 fois au plus un groupe de
    modules qui s'appellent mutuellement.
  - **Écritures de variables globales.** Une seule fusion.
- **Analyse.** Chaque module est ensuite analysé seul, et les résultats sont concaténés.
- **Graphe d'appels.** Aucun graphe ne relie les fonctions de fichiers différents.
  `buildCallGraphFiltered` ([ModulePreparationService.cpp:130](../../../src/analyzer/ModulePreparationService.cpp#L130))
  ne garde que les appelés définis dans le module.

## 4. Invariants

### 4.1 Résumé absent, vide, incomplet

Pour chaque fait, la spec fixe trois points :

- **Le fait exporté.** Il est exporté par le module qui définit la fonction, et la spec dit quelle
  conclusion un appelant peut en tirer.
- **L'absence.** Elle signifie « aucune information » : l'appelant se comporte exactement comme
  aujourd'hui face à une déclaration.
- **La complétude.** Un module n'exporte que ce qu'il a établi. Une analyse arrêtée avant la fin
  (budget, limite d'itérations) n'exporte jamais « aucun effet » : elle exporte l'effet le plus
  large, ou rien.

« Aucun effet identifié » n'est une preuve qu'au terme d'une analyse complète. Un résumé vide issu
d'une analyse incomplète reste une absence d'information.

**L'incomplétude se propage aux appelants.** Le résumé d'un appelant n'est pas plus sûr que les
résumés qu'il a utilisés. Une analyse qui a convergé sur des résumés incomplets reste donc
incomplète. Chaque fait dit comment cette propagation est assurée :

- **`UninitializedLocalRead`.** Un indicateur de complétude, propagé explicitement d'appelé en
  appelant, dans un module comme entre fichiers (§5.3).
- **Déterminisme.** La propagation est dans la définition : un appelant n'est déterministe que si
  tous ses appelés le sont (§7.1).
- **Pile maximale.** « Inconnue » joue ce rôle, et #159 le propage déjà à tous les appelants (§8.3).
- **Faits calculés sans budget** (« pointé `const` », paires `SizeMinusOneWrite`). Leur calcul n'a
  ni budget ni limite d'itérations, il ne s'arrête donc jamais avant la fin. Une paire manquante
  fait au pire perdre un diagnostic, sans en créer.

Un appel vers une fonction qu'aucun fichier de l'exécution ne définit, comme une fonction de la
bibliothèque C, ne rend pas l'appelant incomplet. L'appelant lui applique la même présomption que
dans un seul fichier.

### 4.2 Identité des fonctions

Un fait ne traverse les fichiers qu'entre un appel à une déclaration et la définition du même
symbole externe.

- **Clé : le symbole vu par l'éditeur de liens.** La clé est le nom qu'utilise l'éditeur de liens.
  `llvm::Mangler::getNameWithPrefix` le calcule à partir du nom IR et de la `DataLayout` du module :
  il ajoute le préfixe de la cible et retire le marqueur `\1` d'un nom d'assembleur (`__asm__`).
  - Deux déclarations d'un même symbole sont reliées même si elles l'écrivent différemment, par
    exemple l'une par un nom d'assembleur.
  - Deux symboles distincts ne sont jamais confondus. C'est pourquoi la clé n'est pas
    `canonicalizeMangledName` ([mangle.cpp:84](../../../src/mangle.cpp#L84)), qu'utilisent les
    résumés de ressources et de lectures non initialisées. Cette fonction remplace `St3__1`
    (libc++, `std::__1`) et `St7__cxx11` (libstdc++, `std::__cxx11`) par `St`. Une fonction qui
    prend un `std::__1::string*` et une autre qui prend un `std::__cxx11::string*` y ont donc la
    même clé, alors que ce sont deux symboles d'ABI distincts.
  - La PR 1 fait passer les résumés de lectures non initialisées à cette clé (§5.3). Les résumés de
    ressources gardent la leur (§11).
- **Visibilité.** Une fonction à liaison locale (`static`) n'exporte ni n'importe aucun fait, et ne
  compte pas comme définition d'un nom externe. Aucun autre fichier ne peut l'appeler par son nom.
  Deux fonctions `static` homonymes dans deux fichiers sont deux fonctions distinctes. Aucune ne
  masque la définition d'un symbole externe du même nom.
- **Définition exacte.** Un nouveau fait ne vient que d'une définition exacte
  (`GlobalValue::hasExactDefinition()`), comme dans le canal des fonctions sans retour. Selon
  `GlobalValue.h` de LLVM 20, deux cas sont exclus :
  - Une définition `weak`, `linkonce` ou `common` est interposable. L'éditeur de liens peut la
    remplacer par du code arbitraire (`isInterposable()`).
  - Une définition `linkonce_odr`, `weak_odr` ou `available_externally` n'est pas exacte. La
    variante visible est une implémentation correcte, mais pas forcément celle qui s'exécute. LLVM
    refuse d'en déduire une absence d'effet, avec l'exemple d'un `readnone` inféré à tort.

  Ces définitions sont en pratique `inline`, `template` ou `weak`, et un module qui les appelle les
  définit presque toujours lui-même.
- **Plusieurs définitions d'un même symbole.** C'est le cas, par exemple, de deux programmes
  analysés ensemble. Ce qui est publié doit valoir quelle que soit la définition exécutée. On
  distingue trois sortes de faits :
  - **Garanties universelles.** Ce sont les faits qui peuvent déclencher un diagnostic : « n'écrit
    rien », « déterministe », « pointé `const` », paires `SizeMinusOneWrite`. Chacun doit valoir
    pour toute exécution, donc pour toute définition qui peut s'exécuter.
    - Un nom n'est publié qu'une fois **toutes** ses définitions examinées.
    - Il n'est publié que si chacune de ces définitions est exacte et a le fait. Pour les paires,
      seules celles communes à toutes les définitions sont publiées.
    - Aucune publication ne repose sur une partie des définitions, quel que soit l'ordre dans lequel
      elles sont examinées.
  - **Faits qui ne peuvent que retirer un diagnostic.** Ce sont les écritures possibles de
    `UninitializedLocalRead` : elles sont réunies sur les définitions.
  - **Graphe d'appels.** Il ne reçoit aucun arc vers un symbole défini plusieurs fois.
- **Signature.** Un fait indexé par argument ne s'applique à un appel qu'à deux conditions. La
  définition n'est pas variadique, et l'appel lui passe autant d'arguments qu'elle a de
  paramètres. Sinon, le fait est absent pour cet appel.

### 4.3 Sens des faits

Chaque fait n'affirme que ce que le module qui l'exporte a établi. Les sections 5 à 8 le
définissent fait par fait. En particulier :

- un paramètre pointeur vers `const` est un contrat d'interface, pas la preuve que l'objet pointé
  n'est pas modifié (§6.2) ;
- « déterministe » garde la définition explicite de `isFunctionDeterministic` (§7.1).

### 4.4 Convergence et ordre

Chaque fait est calculé d'une de quatre façons. Le résultat dépend de l'ensemble des fichiers, pas de
leur ordre.

- **Une passe, sans dépendance entre modules** (§6). L'ordre est sans effet.
- **Le plus petit point fixe d'un ensemble fini qui ne fait que croître** (§7), comme le canal des
  fonctions sans retour. On répète des passes jusqu'à ce qu'aucune n'ajoute rien.
  - Chaque passe examine toutes les définitions avec le même ensemble, puis publie les nouveaux
    noms. Aucun module ne voit ce qu'un autre a trouvé pendant la même passe, et un nom n'est
    publié qu'après l'examen de toutes ses définitions (§4.2).
  - Un élément ajouté n'en retire jamais un autre, et l'ensemble est fini : le calcul termine.
  - Ce point fixe est unique. Comme chaque passe examine tous les modules avec le même ensemble, ni
    le résultat ni le nombre de passes ne dépendent de l'ordre des modules.
- **Un calcul sur un graphe, indépendant de l'ordre de visite** (§8). #159 l'a établi pour la pile
  maximale.
- **Le pilote existant des lectures non initialisées** (§5).
  - Les modules d'un même niveau sont construits à partir du même index, puis fusionnés par
    réunion.
  - Il en va de même à chaque itération d'un groupe cyclique.
  - Un groupe qui n'a pas convergé à sa limite est marqué incomplet (§4.1). La convergence ne
    dépend que des fichiers.

Les fichiers sont déjà triés par nom (§3) : l'ordre de la ligne de commande est sans effet. Les tests
vérifient en plus l'indépendance au nom des fichiers (§9).

### 4.5 Un seul fichier

Les nouveaux faits ne servent qu'à un appel vers une déclaration résolue dans un autre fichier de
l'exécution. Ils ne sont construits que quand plusieurs fichiers sont analysés ensemble.

- Une exécution sur un seul fichier n'en construit aucun : son résultat ne change pas.
- Dans une exécution sur plusieurs fichiers, un appel vers une définition du même module est traité
  comme aujourd'hui.

## 5. PR 1 — `UninitializedLocalRead` : « n'écrit rien »

### 5.1 Aujourd'hui

- **Contenu d'un résumé.** Pour chaque paramètre pointeur, le résumé d'une définition donne :
  - les octets qu'elle peut écrire ;
  - ceux qu'elle peut lire avant de les écrire ;
  - deux drapeaux, « écriture inconnue » et « lecture inconnue ».
- **Résumés vides.** Ils sont écartés à trois endroits de
  [UninitializedVarAnalysis.cpp](../../../src/analysis/UninitializedVarAnalysis.cpp) :
  - à l'export, [ligne 3857](../../../src/analysis/UninitializedVarAnalysis.cpp#L3857) ;
  - à la fusion, [ligne 3970](../../../src/analysis/UninitializedVarAnalysis.cpp#L3970) ;
  - à l'import, [ligne 1445](../../../src/analysis/UninitializedVarAnalysis.cpp#L1445).
- **Présomption d'écriture.** Faute de résumé, l'appelant voit une déclaration inconnue et applique
  `declarationCallArgMayWriteThrough`
  ([ligne 2096](../../../src/analysis/UninitializedVarAnalysis.cpp#L2096)). Une fonction qui
  retourne `void`, ou dont le résultat est testé, est présumée écrire par ses arguments pointeurs.
  C'est ainsi que `void peek(int*)`, défini dans un autre fichier, masque la lecture.
- **Liaison.** Seules les définitions à liaison externe sont exportées
  ([ligne 1453](../../../src/analysis/UninitializedVarAnalysis.cpp#L1453)), y compris les
  définitions inexactes. La clé est `canonicalizeMangledName`, qui confond des symboles d'ABI
  distincts (§4.2).
- **Ordre des modules.** Pour ordonner les modules, `collectModuleDefinitions`
  ([AnalyzerApp.cpp:1770](../../../src/app/AnalyzerApp.cpp#L1770)) compte aussi les définitions
  `static`. Une fonction `static` homonyme dans un troisième fichier fait croire à deux définitions,
  et le lien vers la vraie est perdu.

### 5.2 Où l'analyse s'arrête avant la fin

Trois cas, vérifiés dans le code :

1. **Le calcul d'une fonction dépasse son budget.** `downgradeWriteClaims`
   ([ligne 1370](../../../src/analysis/UninitializedVarAnalysis.cpp#L1370)) change ses écritures en
   « écriture inconnue ». Mais il saute les paramètres sans aucun effet, qui restent vides.
2. **La boucle entre les fonctions d'un module s'arrête après 64 tours.** C'est
   `computeFunctionSummaries` ([ligne 3494](../../../src/analysis/UninitializedVarAnalysis.cpp#L3494)),
   et rien n'est marqué.
3. **Un groupe cyclique de modules s'arrête après 12 itérations**
   ([CrossTUSummaryDriver.hpp:224](../../../src/app/CrossTUSummaryDriver.hpp#L224)). Seul un
   avertissement est journalisé, et les résumés sont fusionnés tels quels.

Si l'on gardait les résumés vides sans autre changement, chacun de ces cas deviendrait « n'écrit
rien ». Le cas 1 le fait déjà pour les paramètres sans effet d'un résumé non vide.

Un appelant qui a convergé sur ces résumés en hérite. Dans un module, `g(int *p) { f(p); }`, où le
calcul de `f` n'a pas convergé, a un résumé vide si celui de `f` l'est. Marquer seulement `f` ne
suffit donc pas : il faut marquer aussi tout ce qui a été calculé à partir de `f`.

### 5.3 Changement

- **Indicateur de complétude.** Le type public `UninitializedSummaryFunction` reçoit un indicateur
  `complete`. Le résumé d'une fonction est complet si toutes ces conditions tiennent :
  - son propre calcul a convergé ;
  - la boucle de son module a convergé ;
  - la boucle de son groupe cyclique de modules a convergé, s'il en a un ;
  - **chaque résumé qu'il a utilisé est complet**, celui d'un appelé du même module comme celui d'un
    appelé importé.
- **Propagation.** Une fois la boucle du module terminée, l'incomplétude est propagée par le graphe
  inverse des appels du module (`callerOf`), jusqu'à ce qu'elle ne s'étende plus.
  - Elle part des fonctions dont le calcul n'a pas convergé, et de celles qui appellent un résumé
    importé incomplet.
  - Si la boucle du module n'a pas convergé, toutes ses fonctions sont incomplètes.
  - Si un groupe cyclique n'a pas convergé, toutes les fonctions de ses modules sont incomplètes.
  - Entre fichiers, un appelant construit à un niveau suivant reçoit ces résumés marqués
    incomplets, et devient incomplet à son tour.
- **Résumé complet d'une définition exacte.** Il est exporté tel quel, même vide. Un résumé vide
  signifie alors : n'écrit ni ne lit rien par ses paramètres pointeurs.
- **Usage d'un résumé incomplet importé.** À l'appel, chaque paramètre compte comme « écriture
  inconnue » : c'est ce que fait déjà `downgradeWriteClaims` pour les paramètres qui ont un effet,
  et ce drapeau signifie déjà « ne pas signaler ».
  - Un résumé incomplet ne prouve donc jamais « n'écrit rien ».
  - Les lectures avant écriture déjà trouvées sont gardées, car ce sont des lectures réelles.
- **Résumé complet d'une définition inexacte.** Comme aujourd'hui : un résumé non vide est exporté,
  un résumé vide ne l'est pas. Une définition inexacte ne donne jamais « n'écrit rien ».
- **Symbole défini plusieurs fois.** « N'écrit rien » est une garantie universelle (§4.2), mais le
  pilote ne sait pas placer un appelant après toutes les définitions d'un tel symbole : il ne crée
  aucun arc vers lui ([CrossTUSummaryDriver.hpp:93](../../../src/app/CrossTUSummaryDriver.hpp#L93)).
  - Un appelant pourrait donc voir une définition avant les autres. Le résumé d'un symbole défini
    par plusieurs modules est donc toujours incomplet.
  - `collectModuleDefinitions` connaît toutes les définitions avant la première construction.
  - Les écritures possibles de ces définitions restent réunies, comme aujourd'hui.
- **Fusion.** Les effets sont réunis, comme aujourd'hui, et `complete` est vrai seulement si toutes
  les entrées fusionnées le sont.
- **Import.** Les résumés vides sont gardés.
- **Identité.**
  - La clé devient le symbole vu par l'éditeur de liens (§4.2), à l'export, à l'import et dans le
    pilote.
  - `collectModuleDefinitions` ignore les définitions `static`.
- **Un seul fichier.** L'usage des résumés à l'intérieur d'un module ne change pas : la complétude
  n'y sert qu'à l'export. Le cas 1 de §5.2 reste possible dans un fichier (§11). Ce changement
  l'empêche seulement de traverser les fichiers.

### 5.4 Tests d'abord

À autoriser avec la PR :

- **Paire `test/uninitialized-variable/cross-tu-uninitialized-noeffect-{def,use}.c`**, vérifiée dans
  `check_uninitialized_cross_tu`. Attendu : signalé seul et ensemble, dans les deux passes. Elle
  doit échouer sur `main`.
- **Cas négatifs**, non signalés ensemble :
  - `peek` écrit : la paire existante `cross-tu-uninitialized-wrapper-*` ;
  - `peek` est `weak` et n'écrit rien : définition inexacte ;
  - `peek` est défini dans deux fichiers et n'écrit rien dans aucun des deux. Un symbole défini
    plusieurs fois ne prouve jamais « n'écrit rien » ;
  - `peek` est défini dans deux fichiers et n'écrit que dans l'un d'eux.
- **Identité de liaison.**
  - *Symboles d'ABI distincts*, en IR textuel (`.ll`). La définition prend un pointeur vers un type
    de `std::__1`, et l'appel déclare le même nom avec `std::__cxx11`. Attendu : non signalé, car
    aucun résumé n'est relié. Avec `canonicalizeMangledName`, les deux seraient reliés.
  - *Nom d'assembleur.* L'appelant déclare `peek` sous un autre nom C, avec
    `__asm__(STR(__USER_LABEL_PREFIX__) "peek")`, pour Linux comme pour macOS. Attendu : signalé,
    car c'est le même symbole.
  - *Homonyme `static`.* Un troisième fichier définit un `static void peek(int*)`. Attendu : le
    diagnostic de la paire reste signalé ensemble.
- **Propagation de l'incomplétude**, par des tests unitaires : la limite d'itérations n'est pas une
  option de la ligne de commande. La limite choisie laisse converger une fonction sans boucle, mais
  pas une fonction avec boucle.
  - *Dans un module* : `f(int *p)` contient une boucle et n'écrit rien, `g(int *p) { f(p); }`, et
    `h(int *p)` n'appelle rien. Attendu : `f` et `g` incomplets, `h` complet et vide.
  - *Entre deux modules* : le résumé de `f` est construit dans un module avec cette limite. Celui de
    `g` est construit dans un autre module, avec l'index du premier. Attendu : `g` incomplet.
  - *Groupe cyclique non convergé* : le pilote est testé avec des opérations factices qui ne
    convergent jamais. Attendu : les résumés du groupe sont marqués incomplets.
- **Ordre.** La paire tourne aussi avec des noms de fichiers qui inversent l'ordre de tri.

## 6. PR 2 — `ConstParameterNotModified` : le type des paramètres de la définition

### 6.1 Aujourd'hui

`callArgWriteState` ([ConstParamAnalysis.cpp:325](../../../src/analysis/ConstParamAnalysis.cpp#L325))
tient un argument d'appel pour non écrit dans trois cas :

- l'appelé n'accède pas à la mémoire, ou ne fait que la lire ;
- le paramètre porte `readonly` ou `readnone` ;
- le paramètre de l'appelé est déclaré pointeur, ou référence, vers `const` dans ses informations de
  débogage (`calleeParamIsReadOnly`,
  [ligne 302](../../../src/analysis/ConstParamAnalysis.cpp#L302)).

Le corps de l'appelé n'est jamais lu. En `-O0`, une déclaration n'a pas de `DISubprogram`. Le
dernier critère ne fonctionne donc que si la définition est dans le même module.

### 6.2 Fait exporté

- **Fait.** Pour chaque définition exacte à liaison externe, et pour chaque paramètre `i` : ce
  paramètre est déclaré pointeur ou référence vers `const`. Le test est celui de
  `calleeParamIsReadOnly` : ni pointeur de pointeur, ni `void*`, ni pointeur de fonction.
- **Sens.** L'interface de la définition promet de ne pas modifier l'objet pointé par ce paramètre.
  Ce n'est pas une preuve : la définition peut retirer le qualificatif par une conversion, puis
  écrire.
- **Usage.** À un appel vers une déclaration, un argument qui serait « inconnu » devient « non
  écrit » si le fait importé le dit. C'est exactement ce qui se passe avec une définition du même
  module.
- **Calcul.** Une passe sur tous les modules, sans dépendance entre modules. La publication vient
  après l'examen de toutes les définitions.
  - Un symbole et un paramètre ne sont publiés que si chaque définition du symbole est exacte, a le
    même nombre de paramètres et déclare ce paramètre pointeur vers `const` (§4.2).
  - La condition de signature de §4.2 s'applique à chaque appel.
- **Choix retenu : A1** (§10). Le fait reste ce contrat d'interface, à parité avec l'analyse d'un
  seul fichier. Le message du diagnostic est traité à part (§11).

### 6.3 Tests d'abord

- **Paire dans `test/pointer_reference-const_correctness/`**, avec une vérification sur plusieurs
  fichiers dans `run_test.py`. Attendu : signalé seul et ensemble.
- **Cas négatifs**, non signalés ensemble :
  - le paramètre de la définition n'est pas `const` ;
  - le nombre d'arguments diffère ;
  - un homonyme `static` à paramètre `const` dans un troisième fichier ;
  - le symbole est défini dans deux fichiers, avec un paramètre `const` dans l'un et non `const`
    dans l'autre. Ce cas est testé dans les deux ordres de tri.

## 7. PR 3 — `DuplicateIfCondition` et `SizeMinusOneWrite` : deux faits transitifs

### 7.1 `DuplicateIfCondition` : « déterministe »

La définition est celle de `isFunctionDeterministic`
([DuplicateIfCondition.cpp:439](../../../src/analysis/DuplicateIfCondition.cpp#L439)), inchangée.
Une fonction est déterministe si toutes ces conditions tiennent :

- **Lectures.** Elle ne lit que ses variables locales, la mémoire atteinte par ses arguments
  pointeurs, des globales constantes et des constantes (`isAllowedReadObject`,
  [ligne 417](../../../src/analysis/DuplicateIfCondition.cpp#L417)). Une lecture d'une globale
  modifiable, ou un accès `volatile`, l'exclut.
- **Écritures.** Elle n'écrit que dans ses variables locales.
- **Code opaque.** Elle ne contient ni assembleur en ligne ni appel indirect.
- **Appels.** Elle n'appelle que trois sortes de fonctions :
  - des fonctions déterministes ;
  - des déclarations marquées `noreturn`, qui ne retournent jamais ;
  - neuf fonctions de la bibliothèque C (`isKnownDeterministicDeclaration`,
    [ligne 431](../../../src/analysis/DuplicateIfCondition.cpp#L431)) : `strcmp`, `strncmp`,
    `memcmp`, `strlen`, `strnlen`, `memchr`, `wcslen`, `wcscmp`, `wcsncmp`.

  Tout autre appel vers une fonction inconnue l'exclut.
- **Cycles.** Elle n'est sur aucun cycle d'appels.

**Entre fichiers.** Une déclaration compte comme déterministe si son nom est dans l'ensemble
importé.

**Sens à l'usage.** Les deux conditions appellent la fonction avec les mêmes arguments, et rien
entre elles n'écrit la mémoire que ces arguments désignent. L'appel le vérifie, comme aujourd'hui.
Les deux conditions donnent donc le même résultat.

### 7.2 `SizeMinusOneWrite` : paires (destination, longueur)

- **Fait.** L'ensemble des paires `(dst, len)` d'une fonction
  ([SizeMinusKWrites.cpp:71](../../../src/analysis/SizeMinusKWrites.cpp#L71)).
- **Sens d'une paire.** Sur au moins un chemin, l'argument `len` est transmis tel quel comme
  longueur d'une écriture bornée dont la destination est l'argument `dst`.
  - « Tel quel » : par son emplacement en `-O0`, aux conversions près.
  - Cette écriture est un puits connu (`memcpy`, `memmove`, `memset`, `strncpy`, `strncat`, `stpncpy`
    et leurs intrinsèques), ou un appel à une autre fonction qui a une telle paire
    (`buildSizeMinusKSummaries`, [ligne 348](../../../src/analysis/SizeMinusKWrites.cpp#L348)).
- **Entre fichiers.** Une déclaration reçoit les paires importées pour son nom.

### 7.3 Calcul et convergence

Chacun des deux faits est calculé séparément, comme le plus petit point fixe d'un ensemble qui ne
fait que croître, sur le modèle du canal des fonctions sans retour :

- partir de l'ensemble vide ;
- à chaque passe, chaque module examine ses définitions à liaison externe **avec le même ensemble**,
  celui de la passe précédente. Une déclaration compte d'après cet ensemble ;
- à la fin de la passe, publier les nouveaux faits. La publication ne vient qu'après l'examen de
  toutes les définitions du symbole (§4.2) :
  - un symbole est publié comme déterministe si chacune de ses définitions est exacte et
    déterministe ;
  - une paire est publiée si chacune des définitions du symbole est exacte et a cette paire ;
- s'arrêter quand une passe ne publie rien.

Les propriétés viennent de là :

- **Monotonie.** Une définition déterministe avec un ensemble le reste avec un ensemble plus grand,
  et une paire trouvée le reste. Un fait publié reste donc vrai aux passes suivantes.
- **Terminaison et unicité.** Les ensembles sont finis, et une passe qui publie ajoute au moins un
  élément : le calcul termine. Son résultat est le plus petit point fixe, unique, donc indépendant
  de l'ordre des modules.
- **Examen avec le même ensemble.** Aucun module ne voit, pendant une passe, ce qu'un autre vient de
  trouver. Le nombre de passes lui-même ne dépend donc pas de l'ordre des modules.
- **Cycles.** Un cycle entre fichiers n'entre jamais dans l'ensemble des fonctions déterministes,
  comme dans un seul fichier.
- **Pas d'abstraction commune.** Chaque fait garde son ensemble et sa boucle : seul le schéma est
  repris.

### 7.4 Tests d'abord

- **Une paire de fichiers par règle**, dans `test/diagnostics/` et `test/size-arg/`. Attendu :
  signalé seul et ensemble.
- **Transitivité.** Pour chaque fait, une chaîne sur trois fichiers : `a` appelle `b`, qui appelle
  `c`.
- **Cycle.** Un cycle entre deux fichiers n'est pas déterministe : non signalé.
- **Cas négatifs**, non signalés :
  - une lecture d'une globale modifiable ;
  - un appel inconnu ;
  - un homonyme `static`.
- **Toutes les définitions.** Pour chaque fait, un symbole défini dans deux fichiers, appelé depuis
  un troisième. Chaque cas est testé avec des noms de fichiers qui mettent la définition fautive
  avant, puis après l'autre dans l'ordre de tri.
  - *Déterminisme* : l'une des définitions lit une globale modifiable. Attendu : non signalé.
  - *Paires* : l'une des définitions passe `n` à `strncpy`, l'autre non. Attendu : non signalé.
  - *Témoin* : les deux définitions ont le fait. Attendu : signalé.
- **Ordre.** La même paire, avec des noms de fichiers qui inversent l'ordre de tri.

## 8. PR 4 — Récursion et pile maximale : un graphe d'appels pour tous les fichiers

### 8.1 Articulation avec #159

La PR de #159 calcule deux choses dans chaque module :

- **Les composantes récursives.** C'est `computeRecursiveComponents`
  ([StackComputation.cpp:999](../../../src/analysis/StackComputation.cpp#L999)) : l'algorithme de
  Tarjan, les fonctions `norecurse` exclues.
- **La borne basse et le statut « inconnu » de chaque fonction.** C'est `StackTotals`
  ([ligne 786](../../../src/analysis/StackComputation.cpp#L786)). Le calcul se fait appelés
  d'abord, sans récursion, et ne dépend que du graphe et des cadres, pas de l'ordre des fonctions.

La PR 4 garde ces deux fonctions telles quelles, et leur donne un graphe qui couvre tous les
fichiers.

### 8.2 Graphe

- **Nœuds.** Toutes les définitions de tous les modules, sous forme de `const llvm::Function*`. Les
  modules restent chargés, donc les pointeurs sont stables et distincts d'un module à l'autre.
- **Arcs.**
  - Ceux d'aujourd'hui, à l'intérieur de chaque module.
  - En plus, un arc de l'appel d'une déclaration vers la définition de son symbole dans un autre
    module. Le symbole est celui que voit l'éditeur de liens, et sa définition doit être exacte et
    unique (§4.2).
  - Une fonction `static` n'est jamais la cible d'un arc entre modules.
- **Cadres.** `computeLocalStack` est inchangé, sauf pour un appel résolu ainsi, qui est traité comme
  un appel vers une définition du même fichier :
  - il ne compte plus comme non résolu (`isUnresolvedCall`,
    [ligne 70](../../../src/analysis/StackComputation.cpp#L70)) ;
  - il compte comme un appel en mode ABI (`hasNonSelfCall`,
    [ligne 39](../../../src/analysis/StackComputation.cpp#L39)).
- **IR.** Le graphe est construit après les étapes qui précèdent aujourd'hui le calcul de pile dans
  chaque module : la réécriture des appels sans retour, puis `runFunctionAttrsPass`
  ([AnalysisPipeline.cpp:323](../../../src/analyzer/AnalysisPipeline.cpp#L323)). Il voit ainsi le
  même IR qu'une analyse d'un seul fichier.
- **Résultats.** Quatre résultats passent par `AnalysisConfig` : la borne, le statut inconnu, les
  fonctions récursives et les récursions infinies. L'analyse de chaque module y lit les valeurs de
  ses propres fonctions.

### 8.3 Ce que cela donne

Les définitions de #159 donnent :

- **Cycle entre fichiers.** Ses membres, et les fonctions qui l'appellent, ont une pile maximale
  inconnue, avec une borne basse.
- **`--assume-external-frame`.** L'option ne charge plus que les appels qui restent non résolus.
- **Ordre.** Le graphe est construit par nom, pas selon l'ordre des modules, et #159 rend le calcul
  indépendant de l'ordre de visite.

### 8.4 `Recursion.Unconditional`

Choix retenu : **B1** (§10), dans la PR 4.

Les membres d'une composante peuvent désormais être dans plusieurs modules.
`detectInfiniteRecursionComponent`
([ligne 1053](../../../src/analysis/StackComputation.cpp#L1053)) reconnaît un appel récursif en
comparant l'appelé aux membres de la composante.

- **Résolution.** Le test d'appel récursif résout d'abord une déclaration vers sa définition, avec
  la même identité que les arcs du graphe (§8.2). `leavesThroughNoreturnCall` (#160) reçoit le même
  test.
- **Diagnostic.** Une composante n'est signalée comme récursion infinie que si aucun de ses membres
  n'a de chemin qui en sort : un retour qui ne repasse pas par un membre, ou un appel qui ne
  retourne pas.
- **Parité.** Deux fichiers donnent alors le même diagnostic qu'un seul :

  | Cycle `ping` ↔ `pong` | Un fichier, `main` 071aa37 | Deux fichiers, `main` 071aa37 | Deux fichiers, attendu |
  |---|---|---|---|
  | sans sortie : `ping(n) { pong(n); }`, `pong(n) { ping(n + 1); }` | `Detected` et `Unconditional` sur les deux | rien | `Detected` et `Unconditional` sur les deux |
  | avec une sortie possible : `ping` retourne 0 si `n <= 0` | `Detected` sur les deux | rien | `Detected` sur les deux |

### 8.5 Tests d'abord

- **Une paire pour la récursion, une pour la pile, dans `test/recursion/`.** Elles sont vérifiées
  dans `run_test.py` par le JSON, comme `check_cycle_max_stack` : `maxStack`, `maxStackUnknown`,
  `maxStackLowerBound`, `isRecursive`.
- **`Recursion.Unconditional`**, les deux cycles du tableau de §8.4, chacun seul et réparti sur deux
  fichiers, dans les deux passes.
  - *Cycle sans sortie* : `Unconditional` sur les deux membres.
  - *Cycle avec une sortie possible* : `Detected` seulement.
- **Autres cas :**
  - un cycle entre deux fichiers, pour la pile : membres et appelants inconnus, avec une borne
    basse ;
  - une fonction `static` homonyme dans chaque fichier ;
  - un symbole défini dans deux fichiers : aucun arc vers lui ;
  - le mode ABI ;
  - `--assume-external-frame` ;
  - la même paire, avec des noms de fichiers qui inversent l'ordre de tri.

### 8.6 Limite

`strongConnect` ([ligne 718](../../../src/analysis/StackComputation.cpp#L718)) est récursive
(alerte #227). Sur un graphe qui couvre tout un projet, sa profondeur de récursion croît avec la
plus longue chaîne d'appels.

## 9. Validation (chaque PR)

- **Avant correction.** La nouvelle paire reproduit le défaut sur `main` : signalé seul, pas
  ensemble, dans les deux passes.
- **Après correction.** Elle passe, dans les deux passes.
- **Un seul fichier.** Chaque fixture analysée seule donne les mêmes fonctions et les mêmes
  diagnostics avant et après. La suite complète passe.
- **Plusieurs fichiers.** Lua 5.4.8, tous les fichiers ensemble, est comparé avant et après. Chaque
  différence est expliquée.
- **Ordre.** Les paires tournent aussi avec des noms de fichiers qui inversent l'ordre de tri.
  L'ordre de la ligne de commande est déjà sans effet (§4.4).
- **Tests.** Chaque changement de test est présenté avec ses preuves avant d'être fait.

Cas de validation, par invariant :

| Invariant | Cas | Section |
|---|---|---|
| Absent ≠ vide ; incomplétude propagée aux appelants | module, deux modules, groupe cyclique non convergé | §5.4 |
| Identité de liaison | symboles d'ABI distincts, nom d'assembleur, homonymes `static` | §5.4, §6.3, §7.4, §8.5 |
| Garanties universelles sur toutes les définitions | symbole défini dans deux fichiers, dans les deux ordres de tri | §5.4, §6.3, §7.4 |
| Convergence et ordre | chaînes sur trois fichiers, cycles, noms de fichiers inversés | §7.4, §8.5 |
| `Recursion.Unconditional` | cycle entre fichiers sans sortie, puis avec une sortie possible | §8.5 |

## 10. Choix arrêtés

**A1. `ConstParameterNotModified` : le contrat d'interface seul (§6.2).**

- Deux fichiers concluent comme un seul, et les résultats d'un seul fichier ne changent pas.
- Le message garde sa limite actuelle, traitée par #167 dans les deux configurations.
- **Écartée, A2** : exiger aussi que la définition n'écrive pas par ce paramètre. Cela change les
  résultats d'un seul fichier, ou fait diverger un fichier et deux.

**B1. `Recursion.Unconditional` entre fichiers, dans la PR 4 (§8.4).**

- Les déclarations sont résolues dans le test d'appel récursif.
- Sans cela, une récursion infinie répartie sur deux fichiers serait signalée comme simple récursion
  (`Info`), alors qu'un seul fichier la signale comme erreur.

## 11. Hors périmètre, relevé

- **Le second tableau de #157.**
- **Écritures de variables globales** (#166). Le canal identifie les globales par leur nom brut,
  sans tenir compte de la liaison
  ([GlobalReadBeforeWriteAnalysis.cpp:59](../../../src/analysis/GlobalReadBeforeWriteAnalysis.cpp#L59)).
  Deux globales `static` homonymes dans deux fichiers partagent donc une entrée.
- **Calcul non convergé dans un seul fichier** (#168). Une fonction dont le calcul n'a pas convergé
  garde « n'écrit rien » pour ses paramètres sans effet, et ses appelants du même fichier en
  héritent (§5.2). La PR 1 l'empêche seulement de traverser les fichiers.
- **Message de `ConstParameterNotModified`** (#167). Le message dit « is never used to modify the
  pointed object » ([DiagnosticEmitter.cpp:1040](../../../src/analyzer/DiagnosticEmitter.cpp#L1040)).
  Les variantes des lignes 1021 et 1032 disent la même chose. Ces messages affirment plus que la
  règle ne sait quand sa conclusion repose sur le paramètre `const` d'un appelé, et c'est déjà le
  cas dans un seul fichier.
- **Définitions inexactes.** Leurs résumés non vides de ressources et de lectures non initialisées
  restent utilisés comme aujourd'hui.
- **Résumés de ressources.** Leur clé reste `canonicalizeMangledName` (§4.2).
- **Lectures avant écriture d'un symbole défini plusieurs fois.** Elles restent réunies, comme
  aujourd'hui. Elles peuvent déclencher un diagnostic sans valoir pour toutes les définitions.
- **Fonctions sans retour** (#170). Le canal de #158 publie un nom dès qu'une de ses définitions
  exactes ne retourne jamais, même si une autre définition du même symbole retourne. La règle de
  §4.2 sur les garanties universelles ne s'y applique pas encore.

## 12. Livraison

- **Ce document** entre par une PR `docs(spec)`, avant la première PR de code.
- **Quatre PR**, une par section 5 à 8, dans cet ordre.
  - Chacune est liée à #157 ; la dernière la ferme.
  - Chacune porte le label `enhancement` et est assignée à SizzleUnrlsd.
- **Pour chaque PR**, le défaut est reproduit avant la correction, le comportement vérifié après, et
  le résultat sur un seul fichier conservé (§9).
