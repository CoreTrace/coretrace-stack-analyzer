# Vérification des suggestions const par Clang, version 2 (#217)

Statut : validée à `8b283a8`, puis complétée à la relecture du plan d'implémentation (D17, fixture
de D13), au détail de l'étape 1 (mutualisation limitée au module, §5 et §10), à celui de l'étape 2 (erreurs natives de Clang conservées, §5.3 ; D18, §5.6 et §6), et après la comparaison des corpus (D19, §5.6 et §6). N'autorise ni le code ni la modification des tests : chaque lot présentera ses
tests, avec leurs preuves d'échec, avant de les ajouter. La fusion n'est pas autorisée.
Issue : #217. Liens : #221 (T v1, référence conservée), #167 (faits et contrat const), #197
(identité d'un global entre fichiers).
Références au code : `2a5b0c1`, c'est-à-dire #221 avec `main` b80d127 fusionné. Bilan et preuves :
`coretrace-stack-analyzer-evidence/217/` (`bilan-final-2a5b0c1.md`, `final-review/`, `optin-cost/`).

## 1. Constat

T v1, derrière `--experimental-const-suggestion-check`, vérifie avec Clang chaque suggestion de
`ConstParameterNotModified` : il réécrit en mémoire les déclarations du paramètre, recompile les
fichiers qui voient le changement et compare leurs diagnostics et leur code généré.

Sur `src/`, zlib, tinyxml2 et PCRE2 (même binaire, Linux x86_64), 97 suggestions : 36 vérifiées,
31 gardées non vérifiées, 30 retirées, aucune par un défaut de T. L'auto-analyse coûte +11,3 % avec
l'option, pour un objectif de +10 %.

La revue finale a confirmé dix défauts à `2a5b0c1` (§6), dont deux suggestions fausses affichées
comme vérifiées. Leurs causes sont communes :
1. **Identité approchée.** La fonction est retrouvée par son symbole avant la réécriture, puis après
   par ses noms décodés (`withoutConst`, `variantOf`), sans sa liaison et sans contrôle
   d'ambiguïté : le dernier candidat trouvé l'emporte.
2. **Paramètre.** Son indice source est calculé `debugArgNo − 1 − this` : un paramètre artificiel
   (`vtt`) le décale. La règle elle-même reçoit de `ParameterDebugBinding` un `vtt` typé comme le
   premier paramètre utilisateur.
3. **Plan sans contrôle de portée.** Une modification peut atteindre un autre paramètre (argument de
   macro réutilisé) ou d'autres fonctions (corps de macro mal reconnu).
4. **Retrait sur une différence non expliquée.** « Le code généré change » retire la suggestion,
   même quand rien n'établit une violation.
5. **Unité réduite au fichier.** Une seule commande par fichier ; le type attendu est comparé en
   texte, dans la langue du fichier de définition.
6. **Candidat affiché différent du candidat vérifié** pour `PointerConstOnly` et
   `ReferenceRvaluePreferValue`.

## 2. Objectif et périmètre

**Objectif.** Chaque verdict est justifié et rattaché au candidat exact, aux unités et aux
configurations effectivement vérifiées.

**Dans le périmètre (refonte interne de T) :**
- identités explicites, plan séparé de son exécution, unité définie par un fichier et une
  configuration, faits R, O et K, correspondances établies, collisions, verdicts à trois états,
  trace ;
- étape 2 : D6, D8, D10, D12, D13 et D18 (§6) ;
- étape 3 : les coûts évitables mesurés et une seule construction de l'invocation Clang.

**Hors périmètre :**
- une compilation commune avec coretrace-compiler, un cache persistant ;
- la refonte du comparateur, qui ne reçoit qu'une entrée nouvelle (§5.9) ;
- les configurations multiples par fichier et `ParameterDebugBinding` (P0) : deux changements
  séparés, P0 étant un préalable ;
- des groupes d'édition de liens fournis par la construction ;
- le comportement sans l'option, inchangé.

**Conservé de T v1 :** la séparation entre l'analyse LLVM et la vérification Clang, la réécriture en
mémoire, la prise en compte des autres fichiers, les normalisations et les exceptions acceptées du
comparateur, les tests.

## 3. Définitions

- **Suggestion** : un `ConstParamIssue` : symbole de la fonction, argument, règle,
  `reliesOnCalleeConst`.
- **Candidat** : le type exact proposé pour un paramètre source et l'alternative qu'il représente,
  construit une seule fois par le planificateur à partir des types de Clang :

  | Règle | Candidat |
  |---|---|
  | `Pointer` | pointé const : `const T *p` |
  | `PointerConstOnly` | pointé const, const de tête conservé : `const T *const p` |
  | `Reference` | `const T &r` |
  | `ReferenceRvaluePreferValue` | l'alternative référence constante `const T &r` ; le passage par valeur n'est pas vérifié |

- **Unité** : un fichier et une configuration, identifiée par l'empreinte de sa commande normalisée
  (arguments et répertoire de travail). Le chargement actuel fournit au plus une configuration par
  fichier ; T n'en revendique aucune autre.
- **Identités** :
  - *source* : l'entité (fonction, méthode, constructeur, opérateur…), sa liaison (externe,
    interne, aucune), ses redéclarations (fichier réel, position) et son origine de gabarit ;
  - *paramètre* : son indice dans la liste des paramètres source, et le `ParmVarDecl` de chaque
    redéclaration ;
  - *ABI* : les symboles que CodeGen émet pour chaque variante (`GlobalDecl`), avant et après.
- **Faits R, O, K** : §5.4.
- **Correspondance établie** : un appariement déduit de faits de compilation (identité source, plan,
  contextes d'instanciation), jamais d'un nom décodé.
- **Verdicts** :
  - *vérifiée* : dans chaque unité vérifiée, aucune violation, toutes les correspondances
    établies, et un code égal aux exceptions acceptées près ;
  - *réfutée* : une violation du contrat est établie : erreur de compilation introduite (une fois
    la portée validée), avertissement suivi introduit, ou fait R, O ou K changé ;
  - *indéterminée* : identité ambiguë, correspondance manquante ou seulement supposée, unité non
    vérifiable, portée inconnue ou non respectée, comparaison non concluante, conversion explicite
    de l'adresse sans violation établie, collision possible entre fichiers.

## 4. Sortie et trace

- **Vérifiée** : la suggestion est gardée. Le type proposé affiché est le candidat (D12, D13). La
  ligne « checked … » nomme le candidat, son alternative, la portée (fichiers analysés ou base de
  compilation) et les configurations vérifiées, puis les différences acceptées et la ligne ABI.
- **Indéterminée** : gardée, avec « not checked with Clang: <raison> ». Le type affiché est le
  candidat s'il existe, sinon la proposition de la règle.
- **Réfutée** : retirée des diagnostics publics.
- **Trace** (`--verbose`), pour chaque verdict : la suggestion (règle, fonction et symbole,
  paramètre par son nom et son indice source), la transformation (fichier et ligne, type d'origine
  et candidat, alternative), l'unité et sa configuration, le verdict et sa raison.
- **Sans l'option** : aucun changement.
- **#167** : les lignes liées à `reliesOnCalleeConst` restent inchangées. Un verdict de T porte sur
  le changement de type ; il ne vaut jamais preuve d'absence d'écriture.

## 5. Architecture

Les responsabilités ci-dessous sont séparées dans le code, sans qu'aucune n'exige sa propre classe
ou interface.

**Flux.**
1. Compilation initiale du fichier de définition, et collecte de ses faits « avant ».
2. Planification de chaque suggestion sur cette même compilation.
3. Compilation initiale de chaque autre unité concernée, une fois par unité et configuration,
   réutilisée pour toutes les suggestions du même module analysé. À l'étape 1, la mutualisation
   s'arrête là : T s'exécute une fois par module, et une unité qui voit les changements de deux
   modules est compilée telle quelle une fois pour chacun. C'est moins que la mutualisation prévue
   d'abord, pour toutes les suggestions de l'analyse (§10).
4. Compilation modifiée de chaque unité concernée.
5. Correspondance et jugement par unité, agrégation, rapport.

### 5.1 Prise en charge

Une suggestion devient : le paramètre source (indice fourni par P0, ou « artificiel » ou
« inconnu »), la règle et l'alternative attendue.

### 5.2 Unités

Le fichier de définition (avec la commande de l'entrée analysée), les autres entrées analysées et,
avec `--compile-commands`, les fichiers C et C++ de la base. Une unité par fichier et par
configuration fournie. Une unité sans commande, impossible à prétraiter ou qui ne compile pas telle
quelle est non vérifiable. Une unité voit le changement quand elle lit un fichier réécrit, quand son
code analysé référence l'entité (en tenant compte de la liaison), ou, pour une fonction libre à
liaison externe, quand elle en nomme l'identifiant après prétraitement.

### 5.3 Compilation initiale et faits « avant »

Une compilation par unité et configuration ; pour le fichier de définition, la même compilation
sert au plan. Elle relève :
- les déclarations de l'entité ;
- les faits R, O et K ;
- les symboles définis et leur liaison ;
- les conversions explicites de l'adresse de la fonction ;
- les erreurs et les avertissements suivis ;
- l'IR préparé pour le comparateur.

Les groupes d'avertissements suivis sont activés explicitement par `-W<groupe>`, puis maintenus comme
avertissements par `-Wno-error=<groupe>`, avec la même politique dans les compilations initiale et
modifiée. Les erreurs natives de Clang restent des erreurs. `-Wno-error=<groupe>` rétrograderait
aussi les diagnostics que Clang traite comme des erreurs par défaut. Chaque groupe suivi dont tous
les diagnostics sont des erreurs par défaut, d'après les niveaux que Clang leur donne, est donc
ensuite remis en erreur par `-Werror=<groupe>`. Avec Clang 20, ce sont, en C,
`incompatible-function-pointer-types` et `int-conversion`, et chaque erreur par défaut d'un groupe
suivi appartient à l'un d'eux. Une commande de projet qui désactive un groupe (`-Wno-<groupe>`), ou
qui promeut en erreurs les avertissements (`-Werror`) ou un seul groupe (`-Werror=<groupe>`), ne
change donc ni leur relevé ni leur nature (D6).

### 5.4 Faits R, O et K

Une liste fermée :
- **R, référence** : la déclaration que vise un appel ou une référence : fonctions, spécialisations,
  constructeurs, opérateurs, conversions ; un appel virtuel par sa cible statique. Les opérandes non
  évalués (`sizeof`, `decltype`) et les instanciations sont inclus.
- **O, override** : pour chaque méthode qui porte le nom de l'entité, les méthodes qu'elle surcharge.
- **K, constante** : la valeur de chaque condition `if constexpr`, et l'association que choisit
  `_Generic`.

Rien d'autre n'est relevé : ni le type des expressions, ni les conversions implicites, ni les autres
constantes. Une référence à l'entité réécrite doit viser la même entité avant et après, selon le
plan.

**Clé d'un fait** : l'unité ; la chaîne des contextes d'instanciation (gabarit et arguments) ; la
position source, à la fois d'écriture et d'expansion, ramenée à l'original par les modifications du
plan dans les fichiers réécrits.

### 5.5 Planification

Sur la compilation initiale du fichier de définition :
- **Entité** : localisée par le symbole de la suggestion (CodeGen), puis décrite par son identité
  source.
- **Paramètre** : indice source fourni par P0. Un paramètre artificiel ou introuvable donne
  « indéterminée ».
- **Candidat** : calculé sur le type structuré (§3).
- **Modifications**, sur le `TypeLoc` de chaque redéclaration :
  - un type écrit dans un argument de macro est réécrit là où il est écrit ;
  - un type écrit par une macro qui n'écrit que ce paramètre reçoit `const` devant l'invocation ;
  - une définition de macro, reconnue par le préprocesseur et non par le texte (D11), n'est jamais
    réécrite : indéterminée ;
  - un typedef de pointeur reçoit le candidat imprimé, et un qualificatif écrit à part est traité
    (D10) ; sinon, indéterminée.
- **Portée du plan** : chaque modification ne sert que les paramètres prévus. Aucun autre paramètre
  ni aucune autre déclaration ne s'écrit à la même position (D5). Sinon, indéterminée.

### 5.6 Compilation modifiée et inspection « après »

Les fichiers réécrits sont remappés ; les mêmes faits qu'en 5.3 sont relevés. La portée est vérifiée
dans chaque unité :
- aucune déclaration écrite dans les sources des fichiers réécrits ne change hors du plan dans sa
  forme écrite : ses qualificatifs, ses déclarateurs, les types et les arguments de gabarit que le
  source écrit. Sa composante déduite (`auto`, `decltype`, arguments déduits d'un gabarit de
  classe) n'en fait pas partie : ce que la réécriture y change est une conséquence du plan, que
  jugent les étapes 4 à 7 (D19). Les signatures instanciées (spécialisations de gabarits) qui changent avec la réécriture ne relèvent
  pas de cet invariant : elles sont examinées par les correspondances (5.7), comme dans
  `const-suggestion-deduction-kept.cpp` ;
- aucun type qu'une modification écrit ne sert hors du type des paramètres prévus, dans une
  déclaration comme dans une expression (conversion, `sizeof`, argument de gabarit) ; l'argument
  par défaut d'un paramètre prévu est hors du plan (D19) ;
- chaque redéclaration visée a le candidat de son propre type, qualificatifs de premier niveau
  compris (D18), comparé dans la langue de son unité (D8).

Si les déclarations nécessaires n'ont pas pu être inspectées (erreur fatale, unité incomplète), la
portée est inconnue.

### 5.7 Correspondance

Par unité, entre ses compilations initiale et modifiée, jamais entre deux unités ni entre deux
configurations :
- **Symboles de l'entité** : chaque variante, par `GlobalDecl`.
- **Constructeurs hérités** : il faut savoir quels symboles émis, avant et après, sont les variantes
  héritées du constructeur visé. Technique candidate : les constructeurs implicites qui en héritent
  (`getInheritedConstructor()`), et leurs symboles par CodeGen. Elle doit être démontrée (cas `t8`
  et un cas avec gabarit) avant d'être adoptée ; sans démonstration, ces cas restent indéterminés.
- **Entités imbriquées dans la cible** : lambdas, statiques locales, classes locales et noms RTTI.
- **Contextes d'instanciation dont les arguments changent** : appariés seulement si les faits qui
  les demandent se correspondent (chacun avec son propre contexte), visent le même gabarit avant et
  après, désignent tous la même paire, et forment une bijection. Sinon, indéterminée, et la raison
  nomme le contexte.
- **Entités imbriquées dans une paire établie** (étape 5, P2) : les types imbriqués (lambdas, classes
  locales) de toute fonction déjà appariée, et ce qu'ils atteignent, identifiés par les faits de
  Clang lus avant la destruction de l'AST, jamais par des heuristiques de noms :
  - un membre d'un type imbriqué, par sa nature, sa variante ABI, les positions du type et du
    membre, son nom, `static`, ses qualificatifs et son type ;
  - une spécialisation dont les arguments ou le type contiennent un type imbriqué, par sa variante,
    la position de son patron, les arguments des spécialisations englobantes, les siens et son type
    de fonction ;
  - un membre implicite d'une classe dont la clé contient un type imbriqué, par cette classe, sa
    nature et son nom, sa variante et son type complet, qualificatifs compris.

  Dans une clé, seuls les types imbriqués passent par les paires ; tout le reste (autres arguments,
  qualifications, type) est comparé tel quel. Seuls les symboles que la réécriture renomme y
  prennent part. Les étapes 3, 4 et 5 tournent jusqu'à un point fixe commun. Une clé portée par plus
  d'un symbole d'un côté ne donne aucune paire, et une raison. Une spécialisation dont la
  réécriture change elle-même le type ou un argument reste sans paire. Les globales (vtables,
  typeinfo, constantes `__const`) ne sont pas couvertes. Limite des tests : la nature et le nom d'un
  membre implicite ne sont pas montrés nécessaires (sur les cas réels, la variante ou le type
  suffit à séparer), de même que chacun des nom, `static` et qualificatifs d'un membre de type
  imbriqué ; ils sont gardés et protégés ensemble.

### 5.8 Jugement

Par unité, dans cet ordre :
1. unité non vérifiable : indéterminée ;
2. inspection « après » incomplète, donc portée inconnue : indéterminée ;
3. portée du plan non respectée : indéterminée ;
4. erreur de compilation introduite : réfutée ;
5. avertissement suivi introduit : réfutée ;
6. fait R, O ou K changé, selon des correspondances établies : réfutée ;
7. comparaison de l'IR avec les paires établies : égalité aux exceptions acceptées près, vérifiée ;
   différence que les faits n'expliquent pas, symbole non apparié ou paire seulement supposée,
   indéterminée.

Une conversion explicite de l'adresse est relevée et nommée ; seule, elle donne « indéterminée ».

### 5.9 Comparateur

Inchangé, à une entrée près : il reçoit les paires établies (5.7).
- Ses heuristiques (encodage masqué, `withoutConst`, `variantOf`, clé de gabarit) ne servent plus
  qu'à situer une différence dans la trace, pour les symboles que ces paires ne couvrent pas.
- « Vérifiée » exige que chaque symbole qui disparaît ou apparaît soit couvert par une paire
  établie. Une paire seulement supposée donne au mieux « indéterminée ».
- Une heuristique n'a aucun pouvoir de verdict : elle ne peut ni établir une paire ni en invalider
  une. Seuls des faits insuffisants ou incohérents empêchent d'établir une correspondance ; une
  approximation de nom n'invalide jamais une preuve.
- Les exceptions acceptées restent celles de T v1, chacune vérifiée et listée : le nom que `typeid`
  donne à un type nommé d'après la fonction, et la signature imprimée par `__PRETTY_FUNCTION__`.

### 5.10 Collisions, édition de liens et agrégation

**Collisions.** Après la réécriture, un symbole que prend l'entité est-il défini par une autre
entité ? Une même entité définie dans plusieurs fichiers, comme une fonction `inline` de l'en-tête
réécrit, n'est pas une collision.

| Liaison de l'autre définition | Règle à l'édition de liens | Verdict |
|---|---|---|
| locale (`static`, espace anonyme) | aucune portée entre fichiers | jamais de collision |
| forte, dans le même fichier | redéfinition | erreur de compilation, réfutée à l'étape 4 |
| forte, dans un autre fichier | définition multiple si les deux fichiers sont liés ensemble | indéterminée |
| `weak` | pas d'erreur ; la définition forte l'emporte en silence | indéterminée, en nommant laquelle l'emporterait |
| `weak_odr`, `linkonce_odr` | fusion sous promesse ODR | indéterminée |

Conclure sur l'ODR exigerait l'équivalence des définitions source : T ne l'établit pas, et l'IR ne
tranche pas, ni par son égalité ni par sa différence.

**Déclaration périmée.** Une unité qui déclare l'entité avec l'ancien type sans voir la réécriture
rend la suggestion indéterminée.

**Hypothèse d'édition de liens.** T ne déduit jamais que des fichiers forment un même programme.
Toute raison qui en dépend énonce l'hypothèse et sa portée, par exemple : « `_Z1fPKi` est aussi
défini dans b.cpp ; si a.cpp et b.cpp sont liés ensemble, les deux définitions entrent en
collision ».

**Agrégation.** La suggestion est réfutée si une unité la réfute ; sinon indéterminée si une unité ou
une collision l'est ; sinon vérifiée, dans les unités et configurations vérifiées.

### 5.11 Rapport

Selon §4.

## 6. Défauts confirmés

Cas dans `coretrace-stack-analyzer-evidence/217/final-review/cases/`, observés à `2a5b0c1`. « VP » :
suggestion valide.

| # | Défaut (cas) | Fait observé | Verdict attendu | Test prévu | Étape |
|---|---|---|---|---|---|
| D1 | paramètre artificiel `vtt` d'un constructeur de base (t10) | la règle suggère `vtt` ; T réécrit `x` et l'affiche « checked » ; `x` (variante C2) est retiré | P0 : plus de suggestion sur `vtt`. T : `x` vérifiée. Un paramètre artificiel qui atteindrait T : indéterminée | P0 : test unitaire de la liaison et fixture de la règle. T : fixture du constructeur | P0, 1 |
| D2 | opérateur membre utilisé dans un autre fichier (t1) | retirée : « the methods it overrides change » | vérifiée (VP) | contrôle run_test.py à deux fichiers | 1 |
| D3 | homonymes C `static` et externe (t2, deux sens) | retirée : « declared in b.c / d.c » | vérifiée (VP), dans les deux sens ; README corrigé | contrôle run_test.py, deux sens | 1 |
| D4 | `f(const int*)` défini dans un autre fichier (t9) | « checked in 2 file(s) » ; l'appliquer définit `_Z1fPKi` deux fois | indéterminée, en énonçant l'hypothèse d'édition de liens | contrôle run_test.py : fichiers analysés ensemble, et autre fichier venant seulement de la base | 1 |
| D5 | type écrit dans un argument de macro qui sert deux paramètres (t3/m.c) | retirée : erreur sur `dst` | indéterminée (VP gardée) : type écrit une seule fois pour `src` et `dst` | fixture | 1 |
| D7 | constructeur hérité (t8) | retirée : symboles `CI1`/`CI2` non appariés | vérifiée si 5.7 est démontrée, sinon indéterminée ; jamais retirée | fixture | 1 |
| D11 | `\` suivi d'espaces dans un `#define` (t13) | corps de macro réécrit, puis retirée | indéterminée (VP gardée) : déclaration écrite par une définition de macro | fixture | 1 |
| D14 | correspondance non unique traitée comme une réfutation (`const-suggestion-deduction-refused.cpp`, cas `call`) | retirée | indéterminée (gardée) ; `pick` reste réfutée (condition `if constexpr`) | attente modifiée de la fixture | 1 |
| D15 | conversion explicite de l'adresse traitée comme une réfutation (`cross-tu-const-suggestion-cast.c`) | retirée | indéterminée (gardée), sauf erreur ou effet établi | attentes modifiées des deux contrôles run_test.py qui l'utilisent | 1 |
| D16 | `sizeof(__PRETTY_FUNCTION__)` (`const-suggestion-printed-signature.c`, cas `signature_size`) | retirée : le code généré change | indéterminée (gardée) : la différence n'est expliquée ni par R, ni par O, ni par K ; K n'est pas élargi pour conserver l'ancien verdict | attente modifiée de la fixture | 1 |
| D17 | redéclaration avec l'ancien type dans un autre fichier (`cross-tu-const-suggestion-redecl.c`), relevée à la planification | retirée : le fichier déclare la fonction lui-même, sans l'en-tête | indéterminée (gardée non vérifiée, ce qui ne démontre pas qu'elle est valide) ; la raison nomme le fichier et l'hypothèse d'édition de liens (§5.10) | attentes modifiées de `check_const_suggestion_cross_tu` et de `check_const_suggestion_compilation_database` | 1 |
| D6 | `-Werror` du projet et groupe suivi désactivé (t5) | « not checked: does not compile as it is » | vérifiée (VP) | fixture avec `--compile-arg` | 2 |
| D8 | définition C, utilisateur C++ via `extern "C"` (t4) | « not checked: the rewritten declaration does not get the suggested type » ; témoin C vérifié | vérifiée (VP) | contrôle run_test.py, avec le témoin | 2 |
| D10 | `const` écrit devant un typedef de pointeur (t3/t.c, t3/t.cpp) | réécrit en `const const int *` ; « not checked » | vérifiée (`const int *const`) ; indéterminée si le qualificatif vient d'une macro | fixtures C et C++ | 2 |
| D12 | alternative de `ReferenceRvaluePreferValue` | seul `const T &` est vérifié ; la ligne dit « the suggested type » | vérifiée ; le type affiché et la ligne nomment `const T &`, le passage par valeur n'est pas vérifié | attentes modifiées (`const-suggestion-deduction-kept.cpp` et autres) | 2 |
| D13 | `PointerConstOnly` (t15) | affiche `const int *p`, vérifie `const int *const p` | vérifiée ; le type affiché et la ligne nomment `const int *const p` | nouvelle fixture qui active T, aucune fixture de T v1 ne couvrant `PointerConstOnly` ; le test existant sans l'option reste inchangé | 2 |
| D18 | redéclaration qui diffère de la définition par un qualificatif de premier niveau du paramètre (`int f(int *p);` puis `int f(int *const p) {…}`, et l'inverse), observée à `4fd0dae` (`stage2/observations/toplevel-qualifier/`) | « not checked: the rewritten declaration does not get the suggested type » | vérifiée (VP), dans les deux sens | contrôle run_test.py, deux sens | 2 |
| D19 | déclaration dont le type est déduit d'un appel que la réécriture rend invalide (`publishSingleStoreSlots`, `IntRanges.cpp:314`, relevée par la comparaison des corpus à `48f3ec1`) | gardée : « not checked: the rewrite also changes the type of an unnamed declaration » | réfutée pour l'erreur introduite ; une partie écrite que la réécriture change hors du plan, y compris dans une expression ou un argument par défaut, reste indéterminée | contrôles run_test.py | F |
| D20 | spécialisations et membres implicites qui dépendent d'entités imbriquées dans une autre paire (`parallelFor`, `build`, `isRecursiveCallee`, corpus à `48f3ec1` ; P2a, P2b, P2c) | gardées : « not checked: the generated code changes … symbols no longer used » | appariées par l'étape 5 de 5.7, vérifiées si l'IR concorde ; une clé ambiguë, ou dont la réécriture change une autre partie, reste sans paire et indéterminée | tests unitaires et contrôles run_test.py | F |

**Non retenus :**
- le cache « avant » indexé par l'identifiant (point 9 de la revue) : non reproduit (t12) ; les
  caches de T v2 sont indexés par entité et par configuration ;
- `typedef int *const CP` (t14) : la règle n'y fait aucune suggestion.

**Étape 3, sans changement de comportement :** la copie du scan de chaque unité pour chaque
suggestion, l'absence de cache pour une compilation « avant » qui échoue, et la construction de
l'invocation Clang en double. Les deux coûts ne sont traités que s'ils sont mesurables.

## 7. Tests

- **TDD, défaut par défaut.** Chaque test est soumis avec la preuve de son échec avant d'être ajouté.
- **Non-régression.** Tous les tests de T v1, sauf les attentes à modifier, elles aussi soumises :
  D12, D14, D15, D16 et D17. D13 ajoute une fixture qui active T ; le test existant sans l'option
  reste inchangé.
- **Corpus** (`src/`, zlib, tinyxml2, PCRE2). Chaque suggestion est comparée entre trois sorties :
  sans l'option, T v1 et T v2. Elle est classée vérifiée, gardée indéterminée, ou retirée sur une
  réfutation établie. Chaque disparition par rapport à la sortie sans l'option est expliquée par sa
  violation, et chaque écart avec T v1 l'est aussi. T v1 n'est pas une référence de justesse.
- **Plateformes.** macOS, Linux (WSL), puis la CI avec le tri du code scanning.

## 8. Coût

Le même binaire, sans puis avec l'option, en alternance (sans, avec, avec, sans), sur la même base et
les mêmes configurations :
- l'auto-analyse telle que la CI la lance (4 tranches, 2 à la fois, profil rapide, caches froids) ;
- zlib, tinyxml2 et PCRE2.

Le critère reste +10 % sur l'auto-analyse. La validation de cette architecture ne vaut pas
acceptation d'un dépassement.

## 9. Livraison

1. **P0, `ParameterDebugBinding`** : issue et PR propres, depuis `main`, avec leurs tests. Préalable
   à l'étape 1.
2. **Branche `217-t-v2`**, depuis `2a5b0c1`. T v1 reste une référence immuable (`217-t-v1`, #221,
   bundle). Après la fusion de P0, `main` actualisé y est intégré et l'ensemble est validé.
3. **Étape 1** : flux, identités, plan et portée, faits R, O et K, correspondances, paires établies
   pour le comparateur, collisions, trace ; D1 (partie T), D2, D3, D4, D5, D7, D11, D14, D15, D16
   et D17.
4. **Étape 2** : D6, D8, D10, D12, D13 et D18.
5. **Étape 3** : les coûts mesurés et l'invocation Clang construite en un seul endroit.
6. **Comparaison des corpus, mesure du coût et README** ; une PR en brouillon. La revue finale porte
   sur la base actualisée.

Chaque étape est validée (tests, plateformes, CI) avant la suivante. Les configurations multiples
restent un changement séparé, hors de ce calendrier.

## 10. Limites

- Sans groupes d'édition de liens, une collision entre fichiers reste indéterminée.
- L'ODR n'est pas tranchée.
- Seules les configurations fournies par le chargement sont vérifiées.
- Une suggestion qui change réellement le comportement, d'une façon que R, O et K ne couvrent pas,
  reste affichée comme non vérifiée, sans être retirée : par exemple `signature_size`, où
  `sizeof(__PRETTY_FUNCTION__)` change. C'est la conséquence de la politique choisie : une
  différence d'IR inexpliquée ne suffit plus à retirer une suggestion.
- La technique d'identification des constructeurs hérités reste à démontrer.
- Un succès du cache IR ne fournit pas d'AST : T compile toujours depuis les sources.
- La compilation initiale d'une unité n'est partagée qu'entre les suggestions d'un même module
  (§5, étape 3) : la partager entre modules demanderait les faits d'entités pas encore planifiées.
  Ce compromis, accepté provisoirement pour l'étape 1, réduit la mutualisation prévue ; son coût
  entre modules est mesuré à l'étape 3 (tâche 3.1). Le critère de +10 % sur l'auto-analyse (§8)
  reste inchangé.
