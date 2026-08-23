# V1 ARM build and stability experiment

RepoTraverse v1 validates whether historical ARMCC/ArmClang build contexts can
be reconstructed accurately enough to compare element-level stability with an
existing file-level architectural partition. These commands execute trusted
repository build logic and are intentionally CLI-only.

## Current analytical boundary

Capture, progressive screening, extraction, coverage reporting, caching, and revision
orchestration are implemented. Stability classification is provisional. The pilot
constructs an ordered series for each explicit build variant and translation unit only
after Git and syntax evidence promote a candidate. It does not yet aggregate the same
external element across TUs into one repository-wide revision state.

Until cross-TU element aggregation is implemented:

- provide `build_variant.product`, `build_variant.target`, and
  `build_variant.configuration` for each capture configuration;
- treat missing revision manifests as an evidence gap outside the classifier;
- interpret element counts and classifications as TU-series trial results, not
  repository-wide deduplicated results; and
- treat the ARMCC5/ArmClang6 adapter as partial until it is qualified against the real
  build environment. A compatible LLVM/Clang frontend is assumed.

## Capture and HEAD coverage

Copy `config/experiment.example.json` and set the repository, output,
extractor, configurations, and environment. The output directory must not
already exist for capture and HEAD runs.

```text
repotraverse experiment capture --manifest experiment.json
repotraverse experiment head --manifest experiment.json
```

The configured Make command receives the `CC`, `CXX`, or other listed compiler
variables pointing to `repotraverse-compiler-probe`. Use an object-producing
Make target for capture-only operation. If the real compiler is available, set
`real_compiler` for pass-through capture.

Each compiler invocation is written independently, so parallel recursive Make
does not contend on a shared log. Relative working directories and response
files are retained. Import expands bounded repository-local response files and
marks unknown semantic options as coverage gaps instead of dropping them.

`head-report.v1.json` contains capture status, unique TU contexts, complete,
partial, and failed extraction counts, failure taxonomy, elapsed time, element
count, and artifact size. A context without a project dependency map is
partial even when Clang parses it successfully.
`extractor_concurrency` bounds parallel Clang processes; use `8` on the target
8-vCPU VM and reduce it if peak memory is too high.
When `artifact_cache` is configured, successful manifests are keyed by source
blob, normalized context, captured dependency blobs, and extractor identity.
Contexts without a dependency map are never placed in that cross-run cache.

## Recent first-parent pilot

```text
repotraverse experiment pilot --manifest experiment.json
```

The pilot uses `pilot.revisions` when provided. Otherwise it selects the most recent
`pilot.max_revisions` commits on `pilot.ref` in first-parent order. `pilot.budget` is
required; every stratum must explicitly cap files, syntax transitions, and semantic
elements, and the manifest must cap capture revisions, dependency depth, and induced
elements per transition. There are no hidden analysis-budget defaults.

The pipeline is progressive:

1. Git measures direct file touches, churn, rename/move history, and direct header
   include fanout across the selected history without a checkout.
2. COMMON leakage candidates, VARIABLE detail, low-change VARIABLE stable-island
   candidates, high-impact headers, and deterministic controls are ranked separately.
3. Vendored Tree-sitter C/C++ parsers read both Git blob endpoints and map zero-context
   hunks to syntactic sites. These IDs are provisional candidates, never canonical C++
   identities. `.c` uses the C grammar; C++ extensions and headers use the C++ grammar
   for screening, with the actual captured language mode deferred to Clang.
4. Only promoted paths select captured TUs for Clang. Clang remains authoritative for
   overloads, templates, preprocessing, declaration/definition identity, dependencies,
   configurations, and targets.

`progressive-screening.v1.json` records file facts, syntax snapshots and transitions,
promotion reasons, cap usage, and coverage. `pilot-report.v1.json` embeds that report and
adds semantic `change_evidence`. Parsed sites are cached under `syntax-cache-v1` by
source content, screening language/path, and parser identity; raw source is not stored.
Completed revision reports remain resumable. A revision
with reusable, complete dependency capture uses an exact-path sparse workspace containing
the selected TUs and their project dependencies. Make capture, repository preparation,
and incomplete closures use one temporary full workspace that is removed after its final
lease.

`workspace_max_bytes`, `workspace_max_revisions`, and
`workspace_free_space_reserve_bytes` bound active and retained materialization. Omitted
byte limits use at most 10 GiB while preserving a 5 GiB free-space reserve. A revision
that cannot fit reports `disk_space_insufficient`; it is never treated as complete.

If extraction needs generated files, set a configuration `prepare_command` to a
target that creates them without compiling the full product. It runs when a
previous build context is reused in a fresh worktree.

The pilot reuses the previous captured build context when configured build
files and source topology are unchanged. Within a reused context it re-extracts
changed TUs and TUs whose captured project dependencies changed. A header
change with no dependency map conservatively invalidates every TU. Unaffected
manifests are re-observed at the new revision without rerunning Clang. Capture
or extraction gaps remain explicit. If a syntax or capture cap truncates the selected
history, Git/syntax facts remain usable but `coverage_complete` is false and the
classifier can return only `insufficient_evidence`.

Change origins are not collapsed into one count. `direct_source` and
`own_declaration` are intrinsic changes. An included header change first creates
`upstream_exposure`; it becomes `confirmed_induced` only when the dependent element's
semantic fingerprints change under the same context. Build-context changes and
unattributed semantic changes remain separate, and multiple causes are retained as a
set. Dependency propagation is bounded by the manifest depth and per-transition caps.

When `partition` is present, the pilot also writes
`stability-report.v1.json`. A standalone classification can consume explicitly
ordered bundle series:

```text
repotraverse experiment classify --manifest stability-input.json
```

The classifier always preserves raw implementation, interface, structural,
and lineage evidence. It returns `stable`, `variable`, or
`insufficient_evidence`. Partial extraction, ambiguous lineage, or too few
observable transitions prevents a stable or variable conclusion.

The classifier does not currently consume accepted lineage assertions. It also cannot
distinguish an element that is semantically absent for a successfully analyzed target
from a target/context that was not observed, because the pilot does not yet construct an
explicit expected-observation matrix.

The comparison report contains an agreement matrix, variable elements inside
developer-labeled stable files (`variation_leakage`), and stable elements
inside developer-labeled variable files (`stable_islands`). Developer labels
are a comparison reference, not asserted ground truth.
`cross_variant_facts` separately reports the current opaque semantic states
observed for the same historical element in two or more build variants. It does
not feed those differences into the temporal classifier.

Logical elements and semantic variants are already normalized in TU manifests, and
build variants are explicit. The remaining analytical milestone is the reduction into
cross-TU logical-element revision states. Temporal stability and cross-target
variability remain separate facts.
