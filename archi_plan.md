# Plan

Ce plan vise a reduire le cout algorithmique des analyses LLVM en mutualisant les parcours et les artefacts intermediaires, sans changer la semantique des diagnostics. L'approche priorise un moteur de pipeline avec dependances `requires/provides`, puis un collecteur de faits partage pour diminuer les traverses redondantes.

## Decisions
- Objectif prioritaire: reduction du temps `real` (wall-clock).
- Strategie de delivery: migration en 2 phases.
- Selection des analyses subscriber vs independantes: revue module par module en phase 1.

## Scope
- In: Evolution de l'architecture d'analyse (orchestration des modules, partage de donnees inter-analyses, cache d'artefacts derives, validation perf/fonctionnelle).
- Out: Changement des regles metier de diagnostics, modification des fichiers de tests, migration complete de toutes les analyses en une seule iteration.

## Action items
[x] Phase 1 - Etablir la baseline perf cible `real` (timing + xctrace) et definir KPI primaires/secondaires (`real` prioritaire, `user/sys` en support).
[x] Phase 1 - Ajouter une instrumentation technique des traverses (compteurs `Module/Function/Instruction` par analyse) pour mesurer le niveau de redondance avant refactor. (fait: detail par step via `Traversal estimate detail` + agregation par modele)
[x] Phase 1 - Revoir chaque analyse pour classifier: `subscriber-compatible` vs `independante` (dataflow/fixpoint). (fait: `docs/architecture/pipeline-subscriber-classification.md`)
[x] Phase 1 - Refactorer `AnalysisPipeline` vers un modele `requires/provides` pour declarer explicitement les dependances entre analyses.
[x] Phase 1 - Introduire un squelette minimal `AnalysisArtifactStore` type-safe (API + invalidation par module/config), sans figer tous les artefacts.
[x] Phase 1 - Ajouter un `IRFactCollector` comme premier pass du pipeline (parcours unique Module/Function/BB/Instruction) exposant des `IRFacts` partages.
[x] Phase 2 - Introduire un mecanisme subscriber (Observer) pour les analyses classees `subscriber-compatible`.
[x] Phase 2 - Migrer d'abord les analyses pilotes vers les faits partages (priorite: `StackBuffer`, `ResourceLifetime`, `Uninitialized`; candidats additionnels: `IntegerOverflow`, `GlobalReadBeforeWrite`) et mesurer le gain incremental sur `real`. (fait: `StackBuffer` + `ResourceLifetime` migrees; `Uninitialized` classee `independante` apres validation de non-regression)
[x] Phase 2 - Etendre `PreparedModule`/cache avec artefacts derivables stables (debug index, symbol maps, type facts) avec cle versionnee. (fait: `DerivedModuleArtifacts` schema `derived-module-artifacts-v1`)
[x] Phase 2 - Ajouter des garde-fous d'architecture: tests d'integration pipeline, checks d'invalidation cache, et assertions de non-regression diagnostics. (fait: checks `run_test.py` + isolement des caches cross-TU; suite complete verte)
[x] Phase 2 - Valider avant/apres et rollout progressif derriere feature flag, pilote par la reduction du temps `real`. (fait: flag `CTRACE_PIPELINE_SUBSCRIBERS`, benchmark A/B, validation complete)

## Validation Summary
- Build: `cmake --build build -j4` OK.
- Tests: `python3 -u run_test.py --jobs=4` => `Passed 1661/1661 tests`.
- Benchmark A/B (`scripts/bench/pipeline_subscriber_ab.sh`, 5 inputs):
  - baseline: `real=0.24s user=0.19s sys=0.03s`
  - subscriber: `real=0.24s user=0.19s sys=0.03s`
