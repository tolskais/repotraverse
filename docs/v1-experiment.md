# V1 ARM build and stability experiment

RepoTraverse v1 validates whether historical ARMCC/ArmClang build contexts can
be reconstructed accurately enough to compare element-level stability with an
existing file-level architectural partition. These commands execute trusted
repository build logic and are intentionally CLI-only.

For the staged operating procedure and evidence gates, use
[`poc-runbook.md`](poc-runbook.md).

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

Without `real_compiler`, the probe only materializes expected outputs; it does not
synthesize compiler dependency data. That capture is useful for an interception smoke
test but remains partial. A Clang pass-through run qualifies the Clang PoC path only.
ARMCC/ARMCLANG qualification remains deferred until the real vendor environment and
compatibility layer are available.

Each compiler invocation is written independently, so parallel recursive Make
does not contend on a shared log. Relative working directories and response
files are retained. Import expands bounded repository-local response files and
marks unknown semantic options as coverage gaps instead of dropping them.
The v1 probe still requires the source path to appear directly on the compiler command
line. An invocation whose source exists only inside `@response.rsp` is not identified as
a TU and must be reported as a capture limitation.

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

1. Git measures direct file touches, churn, rename/move history, and a textual header
   include-fanout estimate across the selected history without a checkout. This estimate
   is a screening signal, not a compiler-resolved include graph.
2. COMMON leakage candidates, VARIABLE detail, low-change VARIABLE stable-island
   candidates, high-impact headers, and deterministic controls are ranked separately.
3. Vendored Tree-sitter C/C++ parsers read both Git blob endpoints and map zero-context
   hunks to syntactic sites. These IDs are provisional candidates, never canonical C++
   identities. `.c` uses the C grammar; C++ extensions and headers use the C++ grammar
   for screening, with the actual captured language mode deferred to Clang.
4. Only promoted paths select captured TUs for Clang. Clang remains authoritative for
   overloads, templates, preprocessing, declaration/definition identity, dependencies,
   configurations, and targets.

Clang can assign the same USR to distinct anonymous C/C++ tags. V1 disambiguates those
tags with a repository-relative spelling anchor before deriving the logical element ID.
This prevents one TU manifest from collapsing separate anonymous declarations, while
also making their exact lineage intentionally sensitive to moving that declaration;
such gaps remain evidence limitations rather than classifier certainty.

`progressive-screening.v1.json` records file facts, syntax snapshots and transitions,
promotion reasons, cap usage, and coverage. An identical
revision/partition/budget/parser/screening-engine plan reuses that complete artifact
instead of repeating Git screening and include-fanout work.
`pilot-report.v1.json` links to it and retains a compact promotion/coverage summary rather
than embedding a second copy; it adds semantic `change_evidence`. Parsed sites are cached
under `syntax-cache-v1` by source content, screening language/path, and parser identity;
raw source is not stored.
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

Some Makefiles override `CC`/`CXX` assignments from the environment. Command arguments
may therefore contain `{compiler_probe}`, for example
`["make", "CC={compiler_probe}", "objects"]`. The placeholder is replaced without a
shell. The importer counts source-less compiler checks and link calls as ignored non-TU
invocations. A preprocessing invocation that names a repository source file remains a TU
record. GCC/Clang dependency forms including
`-MF`, `--dependency-file`, and `-Wp,-MD,...` are recognized.

The `capture`, `head`, and `pilot` experiment commands print a compact JSON summary and
artifact path by default. Add `--full-output` when the complete persisted report must
also be written to stdout. `classify` retains its result-oriented JSON output.

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

The default weights and thresholds shown in the example manifest are an explicit
baseline hypothesis for practical experiments. They are not empirical constants or a
production definition of stability.
