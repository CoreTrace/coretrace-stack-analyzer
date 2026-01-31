```ZSH
 ./stack_usage_analyzer ../build/StateMachineNRF52840/ConfState.cpp \                                                          ✹ ✭
  -I../build/StateMachineNRF52840 \
  --only-file=../build/StateMachineNRF52840/ConfState.cpp \
  --only-function="ConfState::handle, toto"
```
--> tous les cas d'erreurs autour du parsing de `--only-function="ConfState::handle, toto"`
Exemple :

`--only-function="ConfState::handle, toto"`
`--only-function="ConfState::handle,toto"`
`--only-function=" ConfState::handle, toto "`
`--only-function="      ConfState::handle   ,toto  "`

Gérer ces cas également :

```ZSH
--only-file ../build/StateMachineNRF52840/ConfState.cpp
--only-function toto
```

```ZSH
-DDEBUG
-D DEBUG
-DVALUE=42
-D VALUE=42
```

---

## Parsing des options (erreurs attendues)

```ZSH
--only-file
--only-func
--only-function
-I
-D
```
Attendu : message "Missing argument for ..." + exit code != 0.

```ZSH
--unknown-option
```
Attendu : "Unknown option: --unknown-option".

---

## Variantes de filtres (fonction)

```ZSH
--only-function=toto
--only-function=toto,InitState::handle
--only-function="toto, InitState::handle"
--only-function "toto, InitState::handle"
--only-func=toto
--only-func "toto"
--only-function=_Z4totov
```
Attendu : match du nom manglé/démanglé + signature ignorée.

Cas sans match :
```ZSH
--only-function=does_not_exist
```
Attendu : "No functions matched filters for: <file>" mais pas d'erreur fatale.

---

## Variantes de filtres (fichier / dossier)

```ZSH
--only-file=../build/StateMachineNRF52840/ConfState.cpp
--only-file ../build/StateMachineNRF52840/ConfState.cpp
--only-dir=../build/StateMachineNRF52840
--only-dir ../build/StateMachineNRF52840
```
Attendu : seules les fonctions du fichier/dossier.

---

## Combinaisons de filtres

```ZSH
--only-file=../build/StateMachineNRF52840/ConfState.cpp --only-function=toto
--only-dir=../build/StateMachineNRF52840 --only-function=InitState::handle
```
Attendu : intersection des filtres.

---

## Multi-fichiers (entrée)

```ZSH
./stack_usage_analyzer ../build/StateMachineNRF52840/ConfState.cpp \
  ../build/StateMachineNRF52840/InitState.cpp \
  -I../build/StateMachineNRF52840 \
  --only-function="toto,InitState::handle"
```
Attendu : sortie séparée par "File: ...", pas d'erreur.

---

## Formats de sortie

```ZSH
--format=json
--format=sarif
--format=human
```
Attendu : JSON/SARIF valides, chemins de fichier présents.

---

## Dump de filtre

```ZSH
--dump-filter
```
Attendu : logs `[filter]` cohérents avec les règles (keep yes/no).

---

## Robustesse compile-arg / include

```ZSH
--compile-arg=-I/opt/homebrew/opt/llvm@20/include
-I../include
```
Attendu : compilation OK si headers présents.

---

## Cas sans filtre (perf)

```ZSH
./stack_usage_analyzer ../build/StateMachineNRF52840/ConfState.cpp -I../build/StateMachineNRF52840
```
Attendu : exécution longue possible (pas de "loop" infinie). Note la durée.
