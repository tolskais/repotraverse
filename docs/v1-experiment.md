# V1 ARM build and stability experiment

RepoTraverse v1 validates whether historical ARMCC/ArmClang build contexts can
be reconstructed accurately enough to compare element-level stability with an
existing file-level architectural partition. These commands execute trusted
repository build logic and are intentionally CLI-only.

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

The pilot uses `pilot.revisions` when provided. Otherwise it selects the most
recent `pilot.max_revisions` commits on `pilot.ref` in first-parent order. Each
revision is isolated in a temporary Git worktree. Completed revision reports
are reused after interruption.

If extraction needs generated files, set a configuration `prepare_command` to a
target that creates them without compiling the full product. It runs when a
previous build context is reused in a fresh worktree.

The pilot reuses the previous captured build context when configured build
files and source topology are unchanged. Within a reused context it re-extracts
changed TUs and TUs whose captured project dependencies changed. A header
change with no dependency map conservatively invalidates every TU. Unaffected
manifests are re-observed at the new revision without rerunning Clang. Capture
or extraction gaps remain explicit.

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

The comparison report contains an agreement matrix, variable elements inside
developer-labeled stable files (`variation_leakage`), and stable elements
inside developer-labeled variable files (`stable_islands`). Developer labels
are a comparison reference, not asserted ground truth.
