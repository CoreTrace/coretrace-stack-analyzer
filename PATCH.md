# Statut des faux positifs (mise a jour)

## Corriges

### 1) `src/analysis/MemIntrinsicOverflow.cpp:71`

Warning corrige:
- `local variable 'classifyByName' is never initialized`

Patch applique:
- `UninitializedVarAnalysis` ignore maintenant les objets C++ vides (ex: lambda sans state)
  en se basant sur la forme IR + metadata debug (pas une heuristique sur un nom de variable).

### 2) `src/analysis/ResourceLifetimeAnalysis.cpp:823, 825, 995, 1007`

Warning corrige:
- `potential read of uninitialized local variable 'out'`

Patch applique:
- verification d'initialisation "padding-aware" dans `UninitializedVarAnalysis`:
  - on valide l'initialisation des octets semantiques (membres) ;
  - les trous de padding de layout ne declenchent plus de faux positifs.

### 3) `src/cli/ArgParser.cpp` (19 warnings)

Warnings corriges:
- `potential UB: invalid base reconstruction via offsetof/container_of`
- `unable to verify that derived pointer points to a valid object`

Patch applique:
- `InvalidBaseReconstruction` utilise maintenant une resolution recursive de sous-objet
  (type + offset + bornes de projection) au lieu d'un test limite au membre top-level.
- Les projections C++ valides sur objets imbriques (`result.parsed.config.*`) ne sont plus
  confondues avec des patterns `container_of`.

## Non-regressions ajoutees

- `test/uninitialized-variable/uninitialized-local-cpp-empty-lambda-capture.cpp`
- `test/uninitialized-variable/uninitialized-local-cpp-default-member-return.cpp`
- `test/offset_of-container_of/gep_nested_subobject_reference_no_diag.cpp`

## Validation

- Verification ciblee sur:
  - `src/analysis/MemIntrinsicOverflow.cpp` -> `warning=0`
  - `src/analysis/ResourceLifetimeAnalysis.cpp` -> `warning=0`
  - `src/cli/ArgParser.cpp` -> `warning=0`
- Suite de regression complete:
  - `./run_test.py --jobs 4`
  - resultat: **413/413 passed**

## Reste connu

- Hors faux positifs: vrai positif conserve `src/analysis/InvalidBaseReconstruction.cpp:188`.
