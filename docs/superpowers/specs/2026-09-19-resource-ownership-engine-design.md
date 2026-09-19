# Moteur d'obligations de libération pour `ResourceLifetime.MissingRelease`

Statut : proposition corrigée après relecture ; référence pour le futur plan d'implémentation,
sans validation anticipée du code ni autorisation de modifier les tests existants.
Remplace : le comptage `acquires`/`releases` de [src/analysis/ResourceLifetimeAnalysis.cpp](../../../src/analysis/ResourceLifetimeAnalysis.cpp) pour la seule règle `MissingRelease`.

## 1. Objectif et périmètre

`MissingRelease` est aujourd'hui décidé par `releases < acquires` sur des compteurs
statiques par emplacement, sans tenir compte du contrôle de flot. `acquire(&h); if (err)
return -1; release(h);` n'est pas signalé. Ce document spécifie un moteur d'obligations de
libération qui tient compte du contrôle de flot, des transferts de propriété et des effets
des appels, livré **complet sur le périmètre suivant, en une fois** :

- acquisitions et libérations conditionnelles ;
- retours anticipés et retours de handles (directs, via slot temporaire, `phi`, `select`) ;
- ré-acquisitions, écrasements de handles, alias locaux ;
- boucles ;
- appels directs modélisés, résumés intra-module et cross-TU, appels non modélisés ;
- sorties normales et exceptionnelles, y compris via `call` susceptibles de lever.

Hors périmètre, inchangés : `DoubleRelease`, `UseAfterRelease`, `ReleasedHandleEscapes`,
`IncompleteInterproc`, `MissingDestructorRelease`, ainsi que leurs compteurs et
heuristiques. Le moteur ajoute des faits ; il n'en retire aucun.

Principe directeur : **aucune approximation délibérément incorrecte.** Les transferts
sur-approchent les comportements compatibles avec l'IR et les contrats de modèles. Cela
n'implique pas l'absence de faux positifs : une fuite possible selon l'abstraction n'est pas
une preuve de chemin concret réalisable. Une information inconnue est distinguée d'une
alternative connue mais conditionnelle ; elle ne vaut ni preuve de fuite ni preuve de
libération. Une analyse interrompue est explicitement incomplète.

## 2. Architecture

Trois composants, dans [src/analysis/](../../../src/analysis/) :

| Composant | Fichier | Dépend de LLVM | Rôle |
|---|---|---|---|
| Collecte des faits | `ResourceFactCollector.cpp` | oui | Reconnaît opérations, emplacements, alias locaux, effets d'appel ; produit un `ResourceFunctionFacts` (CFG abstrait + événements typés par instruction). Réutilise la reconnaissance existante (règles du modèle, `resolveHandleStorage`, `describeMethodClass`). Ne décide jamais d'une fuite. |
| Résolution | `OwnershipEngine.cpp` | **non** | Propage les états par instruction et par arête jusqu'à convergence. Entrée : `ResourceFunctionFacts`. Sortie : états stabilisés par point de programme + statut (complet / incomplet). |
| Diagnostics | dans `ResourceLifetimeAnalysis.cpp` | oui (localisations) | Rejoue les événements sur les états stabilisés, classe, localise, déduplique. |

Le moteur est testable unitairement sur des CFG synthétiques. La collecte produit des
événements d'appel typés et des identifiants stables ; le résolveur applique la version
courante des résumés, produits par le même moteur en mode résumé. Une mise à jour de résumé
ne nécessite pas de refaire la reconnaissance LLVM. Les modèles, types de ressources,
offsets et identifiants de sites ne sont pas remplacés par des noms d'API dans le moteur.

## 3. Domaine abstrait

### 3.1 Ressources et emplacements

Une **ressource concrète** est une instance acquise. Un site d'acquisition (instruction,
index d'effet et type de ressource) peut produire plusieurs instances ; il identifie une
famille abstraite, pas une instance concrète unique. Un **emplacement mémoire** (`StorageKey`
: alloca + offset, champ, global, argument) contient des références à ces instances.

Les états de propriété restent `NotOwned`, `Owned`, `Released`, `Escaped`. Le contenu d'un
emplacement ou d'une valeur SSA est un ensemble de références abstraites, `Null` ou `Unknown`.
L'adresse d'un emplacement est distinguée du handle contenu dans cet emplacement. Un `gep`
calcule une adresse avec offset ; ce n'est pas une copie indifférenciée du handle de base.

Une alternative abstraite contient conjointement :

- les états et nombres d'obligations des ressources (§3.3), leur type et leur provenance ;
- les contenus mémoire et les valeurs SSA pertinentes au point courant ;
- les égalités d'alias établies et les conditions de succès reconnues des modèles ;
- l'information connue ou opaque **par obligation dans cette alternative** (§4.2) ;
- les pertes de référence déjà établies, distinctes des seules références encore utilisables.

L'état d'un point est un **ensemble d'alternatives**. Le join est l'union de ces alternatives,
avec déduplication canonique ; il ne fait pas une union composante par composante qui
détruirait leurs relations. Des ensembles de références peuvent encore sur-approcher des
alias dans une alternative : ils ne constituent pas une preuve d'égalité concrète.
L'entrée accessible sans acquisition est une alternative sans obligation locale ; `⊥` est
l'ensemble vide d'alternatives. Tous les transferts vérifient `transfer(⊥) = ⊥`.

Le vocabulaire abstrait est fini pour les faits collectés : sites, emplacements, types,
conditions reconnues et identités symboliques de frontière. Les offsets non résolus sont
résumés explicitement, sans créer un nombre non borné d'emplacements. Les conditions
arbitraires du programme ne sont pas toutes corrélées. Une condition non démontrée laisse
ses branches possibles. Si le nombre d'alternatives dépasse le budget, §7 s'applique ;
on ne fusionne pas silencieusement les alternatives connues et opaques.

### 3.2 Valeurs SSA et références encore utilisables

Une définition SSA est unique dans l'IR ; son **contenu abstrait** n'est pas calculé une fois
pour toutes. `load`, `phi` et `select` sont réévalués avec les états entrants jusqu'au
fixpoint, y compris dans les boucles. Les `phi` sont évalués simultanément sur l'arête
entrante correspondante avant la fusion des états du bloc.

Une ancienne valeur SSA sans usage futur atteignable ne compte plus comme référence
permettant de libérer une ressource. La collecte fournit une information de vivacité
conservative, avec les usages de `phi` sur les arêtes. Les copies mémoire encore accessibles
et les références exportées sont traitées séparément ; la fin de vie d'une valeur SSA ne
libère pas la ressource. En cas de doute sur l'accessibilité d'une référence, aucune perte
**certaine** n'est déduite de sa disparition supposée.

### 3.3 Boucles : récence et multiplicité

Par site `s`, `R_s.new` représente au plus la dernière instance concrète dans une alternative,
et `R_s.old` résume les instances précédentes. Une acquisition réussie transfère la dernière
instance vers `old`, redirige ses références et crée une nouvelle instance `new`. Une
acquisition échouée ne réalise pas cette rotation. Les alias certainement égaux sont
redirigés ensemble ; partager le seul nœud `old` ne prouve pas que deux handles sont égaux.

Chaque nœud résumé porte notamment un **nombre d'obligations Owned**, et pas seulement un
bit `Owned`. Les nombres utilisent le domaine fini `0, 1, …, K, Many`, où `Many` signifie
strictement plus de `K` et `K >= 2` est un paramètre de précision. Des ensembles de ces
valeurs expriment plusieurs cardinalités possibles. Addition et décrément sont les images
sur-approchées des opérations sur les entiers naturels : par exemple `Many - 1` produit
`{K, Many}` ; une libération unique de `Many` ne produit jamais zéro.

Une libération ou un transfert de propriété retire **au plus une obligation concrète**
correspondant au handle concerné. Si l'appartenance de ce handle aux instances encore Owned
est ambiguë, les alternatives « obligation retirée » et « aucune obligation retirée » sont
conservées. Un handle certainement déjà libéré ne décrémente pas une seconde obligation.
Les obligations connues et opaques, ainsi que les obligations dont la dernière référence
est perdue, restent distinguées lors de l'agrégation ; elles ne sont pas toutes déchargées
par une seule opération sur `old`.

Une mise à jour `state(R_s.old) := {Released}` ou `{Escaped}` est donc interdite sur la seule
preuve `contents(src) == {R_s.old}`. Si toutes les instances sont effectivement libérées,
le compteur peut atteindre zéro ; sinon les obligations restantes survivent, même après
la libération de `new`. La preuve de finitude porte sur ce domaine complet (cardinalités,
contenus, alias, provenance, alternatives), pas sur l'ancienne formule à quatre états.

### 3.4 Mises à jour fortes et faibles

Deux preuves distinctes sont requises :

- **mémoire** : écraser fortement un contenu exige une adresse désignant un emplacement
  concret unique dans l'alternative. Le fait que son ancien contenu soit `{R}` ne suffit
  pas. Une prise d'adresse n'interdit pas à elle seule une cible précise ;
- **propriété** : remplacer l'état d'une ressource exige une instance concrète unique et
  l'identité du handle consommé. Un nœud abstrait singleton peut résumer plusieurs instances.

À défaut, les mises à jour préservent les autres cibles ou instances possibles. Les
transferts des alias et des compteurs doivent être monotones selon leur concrétisation ;
cette propriété est vérifiée sur le moteur isolé (§10).

## 4. Événements et certitude

Chaque événement porte une **certitude** : `Guaranteed`, `Conditional(pred)`, `Unknown`,
et un point d'application précis (instruction ou arête). La collecte les ajoute aux faits
existants ; elle conserve les compteurs des autres règles.

| Événement | Source | Effet sur la ressource / l'emplacement |
|---|---|---|
| `Acquire{site, dest}` | règle ou résultat de résumé | Sur succès, crée une obligation (§3.3). `acquire_ret` définit la valeur SSA du résultat ; `acquire_out` écrit l'emplacement ciblé. Un store ultérieur du résultat est une copie, pas une deuxième acquisition. Sur échec, applique le contrat d'échec, sans acquisition inventée. |
| `Release{src}` | règle ou résultat de résumé | Décharge l'obligation concrète désignée ; cible ambiguë et multiplicité suivent §3.3–3.4. Ne libère jamais toutes les instances d'un nœud résumé par simple remplacement d'état. |
| `Load/Copy/Address{…}` | `load`, `store`, casts, `gep` | Transporte les identités et les alias, avec distinction entre valeur et adresse. Les stores appliquent §3.4. |
| `Overwrite{dst, value}` | store écrasant une référence | Écrit le contenu connu (`Null`, autre référence) ou opaque, puis évalue les références encore utilisables (§8). |
| `Return{src}` | `ret` | Transfère les obligations effectivement portées par le résultat (§6), en respectant leur multiplicité et les alternatives de contenu. |
| `AddressEscape{slot, call}` | adresse accessible à un appel opaque | Rend opaques les contenus et obligations que l'appel peut affecter dans cette alternative ; conserve les branches n'ayant pas exécuté cet appel (§4.2). |
| `UnknownCall{reachable}` | appel sans contrat suffisant | Représente les modifications possibles de sa portée mémoire et de ses ressources ; aucune libération ou fuite n'est prouvée sur la seule base de cet événement. |

### 4.1 Contrats d'API dans les modèles

Un appel direct n'est pas certain par nature. La syntaxe retenue est un qualificatif
optionnel final dans les [modèles](../../../models/resource-lifetime/generic.txt) :

```
acquire_out <pattern> <arg> <kind> [if_ret<op><constant>]
acquire_ret <pattern> <kind>       [if_ret<op><constant>]
release_arg <pattern> <arg> <kind> [if_ret<op><constant>]

op       := == | != | s< | s<= | s> | s>= | u< | u<= | u> | u>=
constant := entier décimal signé | entier hexadécimal | null
```

Exemples : `if_ret==0`, `if_ret!=null`, `if_rets>=0`. Les opérateurs ordonnés expriment
explicitement signé/non signé car les entiers LLVM sont sans signe intrinsèque. `null`
ne s'emploie qu'avec `==`/`!=` sur un retour pointeur. Le parseur puis la validation au
callsite vérifient syntaxe, type, largeur et représentabilité ; un contrat invalide est
signalé et n'est jamais traité silencieusement comme garanti. Aucun `eval` ni nom d'API
spécifique n'entre dans le moteur.

Sans qualificatif, la règle conserve son sens historique : effet garanti **sur retour
normal**, sous l'hypothèse de validité du modèle. Les règles fournies pour les API dont
l'effet dépend du résultat sont auditées et annotées lors de cette livraison. Le succès
conditionnel ne précise pas à lui seul la valeur d'un out-param sur échec : si son contrat
ne garantit pas sa conservation ou une valeur précise, ce contenu devient opaque, sans
supprimer les obligations antérieures.

Un effet conditionnel produit **dès l'appel** les alternatives connues succès/échec, liées
au résultat. Toute instruction intermédiaire voit ces effets. Une comparaison ultérieure
peut sélectionner les alternatives ; elle ne déclenche pas rétrospectivement l'acquisition.
Le suivi traverse les copies, slots, `phi` et `select` quand la relation est prouvée. Si le
résultat n'est pas testé, les deux alternatives restent connues : une acquisition réussie
sans libération peut donc donner `MissingRelease`, sans Info d'incertitude artificielle.

Les qualificatifs enrichissent la sémantique du nouveau moteur. Le comportement historique
des autres règles doit rester isolé et vérifié ; le parseur partagé ne doit pas leur imposer
implicitement une nouvelle politique de diagnostic.

### 4.2 Appels non modélisés

L'incertitude appartient à une obligation **dans une alternative**, avec sa cause. Il n'y
a pas de bit collant global `uncertain(R)` qui interdise tous les diagnostics du site.
Après `if (c) opaque(h); return;`, la branche sans appel conserve son obligation connue.
Elle peut produire une fuite possible même si l'autre branche exige une Info d'incertitude.
Une preuve de libération ultérieure d'une instance identifiée peut décharger son obligation ;
une cause d'incertitude ne condamne pas indéfiniment toute la famille du site.

La portée d'un appel inconnu comprend les arguments, la mémoire transitivement accessible,
les références déjà exportées et les globals accessibles. L'absence d'argument pertinent
ne prouve pas l'absence d'effet sur une ressource accessible globalement. Des attributs LLVM
valides peuvent réduire cette portée, sans être interprétés au-delà de leur contrat :
`nocapture` n'est pas une preuve d'absence de libération, `readonly` sur un argument ne décrit
pas toute la mémoire, et aucun de ces attributs n'implique `nounwind`. La preuve d'absence
d'effet tient compte des alias et des contrats de mémoire, de capture et de libération
applicables. Les ressources sans sémantique mémoire (handles opaques) exigent leurs modèles.

Une cible, un offset ou une portée inconnus restent explicitement opaques ; ils ne sont
ni omis ni assimilés à une libération. Une Info dédiée explique les obligations dont le
sort dépend de cette opacité (§8), sans neutraliser les obligations connues indépendantes.

## 5. Résumés interprocéduraux : transformateurs

Un résumé est une **relation sur l'état abstrait de frontière complet**. Il transforme
ensemble les identités de ressources, les contenus des emplacements et leurs alias, par
sortie normale/exceptionnelle. L'image indépendante de quatre états pour chaque paramètre
ne suffit pas. La frontière inclut les arguments, leurs emplacements accessibles suivis,
les globals pertinents, le résultat, les ressources fraîches exportées et les pertes internes.

```
FunctionOwnershipSummary {
  normal:      BoundaryRelation
  exceptional: BoundaryRelation
  coverage:    SummaryCoverage    // cas d'entrée calculés et statut de convergence
}
BoundaryRelation: BoundaryInput -> set<BoundaryOutcome>
BoundaryInput {
  bindings, resourceStates, multiplicities, aliasRelations, modelConditions, evidence
}
BoundaryOutcome {
  bindingsAfter, resourceStatesAfter, multiplicitiesAfter
  resultReferences, freshResources, lostObligations, evidence
}
Reference := InputResource(symbol) | FreshResource(site, kind) | Null | Unknown
```

Les symboles sont instanciés sur les ressources du caller. Deux arguments aliasés partagent
la même identité ; un effet n'est pas appliqué deux fois comme s'ils étaient indépendants.
Si leur alias est seulement possible, les configurations compatibles sont considérées
ensemble ou sur-approchées avec un statut explicite si l'analyse n'aboutit pas. Types,
offsets et contenus après écriture font partie du résumé, pas seulement un booléen d'acquisition.

Un `ret` du callee termine ses obligations locales exportées vers le caller ; il ne marque
pas automatiquement `Escaped` la ressource du caller. `identity(h) { return h; }` exporte
`InputResource(arg0)` dans `resultReferences`, sans consommer l'obligation de l'appelant.
Les ressources d'entrée simplement empruntées ne créent pas une obligation locale nouvelle
du callee. Les contrats de consommation ou de transfert explicites restent distingués.

| Comportement du wrapper | Relation attendue chez le caller |
|---|---|
| libère toujours | obligation de l'instance d'entrée déchargée sur chaque sortie normale |
| libère parfois | alternatives connues avec et sans libération |
| libère puis acquiert dans le même slot | ancienne obligation déchargée ; slot lié à une nouvelle ressource sur succès |
| acquiert puis libère une ressource temporaire | aucune obligation temporaire restante ; entrée inchangée si elle n'est pas écrasée |
| acquiert puis libère en écrasant un slot d'entrée | obligation de l'ancienne ressource non libérée ; contenu final du slot décrit, autres alias préservés |
| retourne un argument ou le copie dans un out-param | référence à l'identité d'entrée ; aucune acquisition fictive |

Le moteur calcule la relation pour les configurations abstraites d'entrée pertinentes,
normalisées (y compris leurs alias). Une représentation paresseuse ou symbolique est permise,
mais sa couverture est explicite : une entrée non calculée déclenche le calcul ou produit
un résultat incomplet, jamais un transformateur identité implicite. L'ensemble des symboles
et des configurations est fini ; les ressources créées aux appels réutilisent un espace
fini de sites abstraits avec §3.3, sans concaténer indéfiniment les piles d'appels récursifs.

Composition = composition des relations après substitution des identités et contenus ;
join = union des résultats pour une même entrée. La normalisation conserve les relations
d'alias et la provenance des résultats. Deux notions sont explicitement distinctes :

- transformateur identité : laisse l'état de frontière inchangé sur une sortie normale ;
- relation `⊥` : aucune sortie représentée. Pendant le calcul récursif, ce résultat est
  provisoire ; il ne devient une conclusion qu'après convergence de la SCC.

Les SCC récursives sont calculées par itération monotone depuis `⊥`, sur les configurations
d'entrée concernées. Une fonction sans retour normal n'a pas un effet identité. Les
diagnostics dépendant de ces résumés attendent la convergence de toutes leurs dépendances.

**Cross-TU** : `ResourceSummaryIndex` gagne les relations et leur couverture ; le schéma
de cache passe à `cross-tu-resource-summary-v3`, invalidant les entrées v2. La liste
`effects` existante est conservée pour les autres règles. Fusion, égalité, hash, sérialisation
et `computeChangedResourceFunctionNames` incluent contenus, alias, multiplicité, provenance
et couverture. La clé inclut les contrats et paramètres de précision utilisés.

L'ordonnancement SCC de [AnalyzerApp.cpp](../../../src/app/AnalyzerApp.cpp) est réutilisé,
mais ses plafonds locaux et globaux suivent §7. Un changement de relation invalide les
callers concernés. Seuls des résumés stabilisés et complets pour leur couverture annoncée
sont réutilisables comme tels en cache ; une couverture absente n'est pas un effet vide.

## 6. Retours et transferts de propriété

Un store dans un **slot local de retour** ne transfère pas la propriété : il modifie son
contenu. Le transfert a lieu au `ret` qui retourne effectivement la référence. Une écriture
via out-param ou vers un emplacement non local a sa propre sémantique d'export, représentée
dans la frontière du résumé ; elle n'est pas assimilée à un slot temporaire de retour.

- `result = h; if (c) result = NULL; return result;` conserve les alternatives liées aux
  deux contenus : l'une exporte l'instance, l'autre conserve l'obligation et peut fuir.
- `phi` sélectionne son opérande sur chaque arête avant le join (§3.2). La relation entre
  l'état de propriété et la valeur sélectionnée est conservée dans l'alternative.
- `select(c, h, NULL)` conserve les deux alternatives si la condition n'est pas connue ;
  si elle est prouvée, seule la valeur correspondante est retenue.
- Le retour d'une référence à `old` n'exporte pas toutes ses instances (§3.3). Les champs
  d'un résultat agrégé sont traités séparément, en respectant leurs alias.
- Plusieurs retours source fusionnés en un seul `ret` n'imposent aucune hypothèse sur
  la forme de l'IR ; les écritures intermédiaires et les valeurs SSA restent suivies.

Si une opération ou un contenu ne peut être interprété, son alternative est opaque ; les
autres restent analysables. Un slot préparé puis abandonné par une exception n'exporte pas
sa ressource. Cependant l'appel ayant levé peut lui-même avoir libéré ou exporté celle-ci :
c'est son effet exceptionnel, et non le store précédent, qui décide de l'état résultant.

## 7. Exceptions, sorties, convergence

La sémantique de référence est celle des attributs et instructions de la
[LangRef LLVM 20](https://releases.llvm.org/20.1.0/docs/LangRef.html#function-attributes).

**Sorties vérifiées** : `ret`, `resume`, `cleanupret`/`catchswitch` avec `unwind to caller`,
et sorties exceptionnelles implicites des `call` pouvant propager hors de la fonction.
Les appels directs et indirects sont concernés ; un callee sans corps peut avoir un contrat.
La langue du fichier source ne suffit pas à décider : le test « C : aucun appel exceptionnel »
est remplacé par des tests des attributs et chemins réellement présents dans l'IR.

Pour `call` comme pour `invoke`, on part de l'état juste avant l'appel, **après** les
instructions précédentes. Le même opérateur d'appel applique :

- sur continuation normale, la relation `normal` ou le contrat normal du modèle ;
- sur continuation exceptionnelle, la relation `exceptional` ou une sur-approximation
  explicite des effets avant exception. Une acquisition via out-param ou une libération
  peut avoir eu lieu ; une valeur de retour n'est pas transmise sur cette arête.

Pour `invoke`, le résultat exceptionnel va au successeur d'unwind et traverse ses cleanups.
Pour `call`, il devient une sortie implicite si l'IR permet la propagation vers le caller.
Un effet de modèle conditionné par une valeur de retour normale ne prouve rien, à lui seul,
sur l'effet avant exception : un contrat exceptionnel ou un résumé doit l'établir ; sinon
les obligations concernées portent cette incertitude. Une fonction qui acquiert via un
out-param puis lève doit exporter cette acquisition dans son résumé exceptionnel.

`noreturn` retire **uniquement la continuation normale**. Il n'implique pas `nounwind` :
une fonction qui lève systématiquement garde son chemin exceptionnel. `unreachable` et
les chemins prouvés d'arrêt du processus ne constituent pas une sortie exigeant une
libération dans ce contrat. Une fonction `nounwind` n'a pas de propagation synchrone vers
son caller, mais ses `invoke`, handlers et cleanups internes éventuels sont toujours analysés.
La présence de `nounwind` ne supprime aucun diagnostic de fuite sur ses sorties normales.
Les mécanismes d'exception LLVM non pris en charge, notamment une sémantique asynchrone
spécifique à une cible, doivent produire un statut incomplet explicite plutôt qu'être ignorés.

**Convergence** : worklist initialisée en RPO ; ordre par inclusion des concrétisations,
join des alternatives et transferts monotones. La récence, les compteurs et les substitutions
de résumés font partie des propriétés à vérifier, et ne sont pas présumés monotones sur
la seule base de leur syntaxe. Le domaine et les relations de frontière sont finis pour
les paramètres de précision choisis ; les budgets limitent le coût, pas la définition du fixpoint.

Les budgets configurables distinguent traitements de la worklist, nombre d'alternatives,
configurations de résumé et itérations de SCC/cross-TU. Copier un nombre de tours d'une
autre analyse sans définir l'unité de travail n'est pas un contrat de convergence. Les
limites et paramètres employés apparaissent dans les résultats de couverture et les caches.

Si un budget est épuisé avant stabilité : `incomplete = true`, aucun nouveau `MissingRelease`
n'est déduit du résultat partiel de cette fonction, et une Info `ResourceLifetime.AnalysisIncomplete`
identifie la cause. Son résumé expose une **relation opaque sur toute sa frontière affectable**
(paramètres, mémoire accessible, globals, résultat et ressources nouvelles possibles), pas
seulement un état inconnu sur les paramètres. Les callers héritent de cette opacité sur les
alternatives concernées ; leurs obligations indépendantes restent analysables. Une limite
globale/SCC déclenche la même politique pour toutes les conclusions qui en dépendent.

## 8. Diagnostics

Les diagnostics sont produits après convergence de la fonction et des résumés dépendants,
en rejouant les événements dans l'ordre des instructions sur les états stabilisés. Une
visite supplémentaire de la worklist ne crée ni acquisition ni diagnostic supplémentaires.

**Perte de référence** : après écrasement ou fin de vivacité pertinente, examiner les
références encore utilisables (§3.2) vers les instances dont une obligation subsiste. Une
ré-acquisition n'est pas à elle seule une fuite : `saved = h; acquire(&h); release(saved);
release(h)` reste silencieux quand les contrats des appels l'établissent. La rotation
abstraite `new → old` n'est pas non plus une perte de référence.

Une perte certaine exige une obligation connue présente, une opération et une cible
établies, et la preuve qu'aucune référence restante ne permet d'atteindre l'instance
concernée dans toutes les alternatives pertinentes. Pour `old`, un contenu `{old}` ne
suffit jamais à identifier cette instance (§3.3). Une perte possible est signalée lorsque
les alternatives connues la permettent ; une perte reposant exclusivement sur un effet
opaque ne produit pas `MissingRelease`. Les pertes établies sont conservées même si une
acquisition ultérieure est libérée ; leurs compteurs font partie des obligations Owned,
sans compter deux fois la même obligation dans le verdict final.

**À chaque sortie**, distinguer :

- obligations connues restantes dans une ou plusieurs alternatives : fuite possible selon
  l'abstraction, même si d'autres alternatives sont opaques ;
- obligations restantes sur toutes les alternatives de toutes les sorties atteintes, avec
  couverture complète et sans incertitude affectant cette conclusion : fuite certaine selon
  l'abstraction ; aucune quantification sur un ensemble vide de sorties ;
- obligations dont le sort dépend seulement d'effets opaques : information insuffisante,
  distincte d'une preuve de fuite et d'une preuve de libération.

La règle de fuite reste `ResourceLifetime.MissingRelease`, sévérité Warn. Message historique
« is not released in this function » uniquement lorsque la seconde condition est établie ;
sinon « may leave the function without being released ». Une ré-acquisition avec perte
établie utilise « loses the previous handle » ; une perte seulement possible utilise
« may lose the previous handle ».

Une issue de fuite par (site d'acquisition, type de ressource, fonction), en regroupant
`new`/`old`. Une perte de référence localisable a priorité sur une fuite à la sortie ; cette
priorité ne renforce jamais le degré de certitude du constat choisi. Le choix du témoin
utilise un ordre stable (instruction, localisation), pas l'ordre d'une table de hachage.
Localisation principale : acquisition ou perte ; la sortie peut être associée si sa ligne
est connue et pertinente, différente de la seule fin de fonction. En l'absence de preuve
de cette distinction, omettre la localisation secondaire, sans fabriquer de ligne source.

**Choix retenu pour l'incertitude** : Info dédiée `ResourceLifetime.AnalysisUncertain`, avec
cause et périmètre affecté ; `AnalysisIncomplete` est réservé à l'analyse non aboutie (§7).
Ces identifiants sont distincts de `MissingRelease`. `IncompleteInterproc` et ses mécanismes
restent inchangés. Une Info et une fuite peuvent coexister pour des alternatives différentes ;
la déduplication ne doit pas effacer la fuite connue sous prétexte qu'une Info existe.

La certitude ne se déduit pas de la seule présence de `Owned` ni d'un témoin dont les
conditions sont contradictoires. Une corrélation non démontrée peut causer un faux positif
**possible**, jamais être présentée comme une preuve de fuite concrète.

## 9. Compatibilité

Objectif à vérifier, pas promesse : les fixtures `test/resource-lifetime/` sont rejouées ;
toute reclassification est listée avec sa cause et une reproduction avant modification.
Le renommage prévu est `acquire-returned-conditional-no-leak.c` →
`acquire-returned-conditional-leak.c` : le chemin `return NULL` conserve l'obligation si
l'acquisition a réussi. Comme toute modification d'un test existant, ce changement exige
l'autorisation explicite de l'utilisateur accompagnée de la preuve ; la correction de ce
document n'autorise pas à modifier ou renommer la fixture.

Les nouveaux faits et modèles enrichis ne remplacent pas les données consommées par les
autres règles. Leurs résultats sont comparés au point de référence avant bascule. Les
changements du cache et des API de résumé restent compatibles avec les consommateurs
existants via la conservation des effets historiques, sans attribuer aux anciennes listes
la précision des nouvelles relations.

## 10. Validation

Les attentes sont indépendantes de l'implémentation. Pour isoler un comportement de propriété,
les tests précisent aussi les contrats d'exception des appels ; un diagnostic exceptionnel
supplémentaire ne doit pas être confondu avec l'échec du scénario normal testé.

**Moteur isolé**, CFG synthétiques sans LLVM :

- entrée sans obligation, stricité sur `⊥`, acquisition inaccessible, join des alternatives ;
- vérification sur petits domaines de la monotonie et de l'inclusion des comportements
  d'un interprète concret de référence dans le résultat abstrait ;
- écrasement mémoire fort/faible, alias certains/possibles, valeurs SSA vivantes/mortes,
  réévaluation des `load`/`phi` en boucle et `phi` simultanés par arête ;
- récence, compteurs `0..K/Many`, conservation des obligations antérieures : après au moins
  trois acquisitions au même site, libérer ou retourner une ancienne instance ne décharge
  pas les autres ; répéter une libération du même handle ne décrémente pas une autre instance ;
- boucles équilibrées, acquisitions accumulées, pertes persistantes après libération de
  la dernière instance, frontières de saturation des compteurs ;
- contrat conditionnel testé ou ignoré, effet visible avant la comparaison, branche d'échec
  sans ressource fraîche et contenu du out-param selon son contrat ;
- branche opaque fusionnée avec branche de fuite connue : fuite possible conservée et Info
  distincte ; obligations opaques et connues du même site non confondues ;
- relations de frontière : retour d'argument, alias entre arguments, out-param aliasé,
  composition, remplacement de slot, origine des ressources fraîches ;
- identité contre `⊥`, SCC récursives avec/sans sortie, couverture de résumé manquante ;
- opérateur d'appel partagé par `call`/`invoke`, résultats normaux/exceptionnels différents,
  `noreturn` pouvant lever, budget local/SCC/global épuisé et propagation de l'incomplétude.

**Fixtures LLVM/C/C++**, preuve RED puis GREEN :

- early return ; release conditionnel ; release sur toutes les branches (silence) ;
  acquisition conditionnelle sans release ; boucle acquire/release équilibrée (silence) ;
- ré-acquisition en boucle, suivie de release, précédée d'une acquisition conditionnelle ;
  alias conservant l'ancienne ressource (`saved = h; acquire(&h); release(saved); release(h)`)
  silencieux ; copies mortes ne masquant pas la perte de référence ;
- anciennes instances multiples au même site : une seule libération/échappement ne masque
  pas la fuite des autres ; ancien handle libéré deux fois ne libère pas une autre instance ;
- retour conditionnel via slot commun, slot écrasé, `phi`, `select`, résultat agrégé et
  retour d'argument via wrapper ; test du renommage soumis à l'autorisation prévue §9 ;
- contrat `if_ret` : échec sans acquisition, succès libéré, résultat ignoré sans libération
  (fuite possible, pas d'Info pour la seule absence de test), intermédiaire avant le `br`,
  copies du statut et modèles invalides (erreur explicite) ;
- wrappers libérant toujours/parfois, libérant puis acquérant, acquérant puis libérant,
  avec et sans écrasement de l'entrée ; deux arguments aliasés et retour de la même ressource ;
- appel opaque sur une seule branche puis retour commun : fuite de la branche indépendante
  maintenue ; ressource accessible via un global affectée par un appel sans argument ;
- effets normaux/exceptionnels distincts, acquisition via out-param puis exception dans
  un callee analysé, `call` sans `try`, `invoke` avec cleanup, `noreturn` levant ;
- `nounwind` : absence de propagation interdite, mais fuite normale toujours détectée ;
  vérifications par attributs IR pour les entrées C comme C++, sans filtre par extension ;
- récursion, cross-TU, cache v3 et invalidation v2, dépendances et couverture de résumés ;
  budget épuisé : Info explicite et maintien des conclusions indépendantes chez les callers.

**Condition de livraison** : tests ciblés verts, non-régression complète et reclassifications
approuvées, comportement des autres règles inchangé, résultats stables avec/sans cache et
pour différents nombres de jobs. Les six corrections sémantiques (§3 à §7) doivent chacune
avoir un contre-exemple reproduit et une validation. Aucun test ne doit être affaibli pour
faire passer la nouvelle abstraction.

## 11. Livraison

Une issue. Une branche d'intégration `feature/resource-ownership-engine`, alimentée par des
PR de phase relues séparément, chacune complète et verte sur son périmètre :

1. moteur isolé + tests unitaires ;
2. collecte des faits (emplacements, alias, retours, appels) ;
3. transformateurs, résumés intra-module et cross-TU v3 ;
4. exceptions ;
5. diagnostics, bascule de `MissingRelease`, fixtures, reclassifications.

`main` ne reçoit que la PR finale `feature → main` : il ne porte jamais l'état intermédiaire.
Bascule de `MissingRelease` uniquement dans la phase 5.

Ce découpage organise la relecture ; il ne constitue pas une livraison de fonctionnalités
incomplètes aux utilisateurs. Les propriétés du domaine, des résumés et des exceptions
sont définies avant les phases qui les consomment. Les PR de phase visent la branche
d'intégration et la livraison complète reste conditionnée à §10.

La présente modification porte uniquement sur la spec. Création d'issue, branche, commits,
PR et implémentation seront exécutées dans le cadre de la demande de développement ;
elles ne sont pas déclenchées par la relecture de ce document.
