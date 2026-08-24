# Element History Analyzer

Repotraverse captures compiler-derived evidence for C/C++ source elements across Git
revisions. Git remains the source of exact changed code; artifacts contain compact
fingerprints, locations, lineage candidates, transition facts, and factual history
statistics. The service and capture infrastructure are implemented in v1. The
multitarget, element-level historical aggregation described below is the next
analytical milestone; the current pilot classifier is provisional.

## Windows VM build

Windows builds use `clang-cl` from a prebuilt LLVM/Clang SDK. Run the build from
an initialized x64 Visual Studio developer environment so the Windows SDK,
linker, and runtime are available. CMake and Ninja must also be on `PATH`.

Repotraverse uses UTF-8 for command data, JSON, Git paths, and persisted text.
Windows filesystem, command-line, environment, and process boundaries are
converted to native UTF-16, so paths may contain non-ASCII characters. JSON
files may be UTF-8 (with or without a BOM) or BOM-marked UTF-16; redirected
stdin and stdout are UTF-8 byte streams.

The SDK must contain:

```text
<llvm-root>/bin/clang-cl.exe
<llvm-root>/lib/cmake/llvm/LLVMConfig.cmake
<llvm-root>/lib/cmake/clang/ClangConfig.cmake
<llvm-root>/lib/clang/<major>/include/
```

The official `clang+llvm-<version>-x86_64-pc-windows-msvc` archive is the
reference Windows SDK. Its CMake packages export component targets;
Repotraverse links `clangTooling` and `clangIndex` through those imported
targets so their transitive libraries and Windows compile definitions are
preserved. SDKs exporting the monolithic `clang-cpp` and `LLVM` targets remain
supported on other platforms and as a Windows fallback.

Windows builds are x64-only. The packaged executables embed a
`longPathAware` manifest and Git subprocesses enable `core.longpaths`, but the
host must also enable the Windows **Enable Win32 long paths** policy (or the
equivalent `LongPathsEnabled` registry value). Clang builtin resource headers
are copied into the package under `lib/clang/<major>/include` so the extractor
does not depend on the build SDK after installation.

For a portable executable:

```powershell
.\tools\build-windows.ps1 -Mode Generic -LlvmRoot C:\llvm-sdk
```

For an executable that remains on the VM where it was built:

```powershell
.\tools\build-windows.ps1 -Mode Native -LlvmRoot C:\llvm-sdk
```

Native mode passes `-march=native` through clang-cl. Its executable must not be
copied to a VM that may expose fewer CPU features. Both modes use at most two
build jobs by default; pass `-Jobs N` to override this.

The build is offline: CLI11 2.7.0, nlohmann/json v3.12.0, SQLite 3.53.4,
xxHash 0.8.3, Tree-sitter 0.26.11, the generated C 0.24.2/C++ 0.23.4
grammars, and the Catch2 3.15.0 test distribution are vendored. CMake contains
no dependency download or parser-generation path. Upstream license and
public-domain notices are retained under `third_party/` and installed with the
tool.

The core-only preset still uses clang-cl as the compiler but does not link the
Clang libraries:

```powershell
.\tools\build-windows.ps1 -Mode CoreOnly -LlvmRoot C:\llvm-sdk
```

## Developer build

The extractor is omitted by default. Non-Windows developer machines can build
it by explicitly selecting an installed LLVM/Clang CMake package:

```sh
cmake -S . -B build -G Ninja \
  -DLLVM_DIR=/usr/lib/llvm/22/lib64/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm/22/lib64/cmake/clang
cmake --build build
ctest --test-dir build --output-on-failure
```

Without `Clang_DIR` or `REPOTRAVERSE_LLVM_ROOT`, configuration does not search
for LLVM/Clang and builds only the core executables.

## Extract snapshots

`clang-extractor` is the compiler-facing helper used by the background worker.
It can also be run directly for diagnostics. A compilation database may be
supplied with `-p`.

```sh
clang-extractor -p build \
  --source-revision <revision> \
  --configuration <configuration> \
  --context-fingerprint <compile-command-hash> \
  source/device.cpp > device.json
```

Persistent TU manifests do not contain function bodies, expressions, source
snippets, or AST edit scripts. They contain logical elements, semantic variants,
source locations, resolved dependency identifiers, and producer versions.
Project paths are repository-relative. `file.history` deduplicates identical logical
elements and variants across the endpoint manifests it queries while retaining their
supporting configurations, contexts, and TUs. The experimental pilot classifier does
not yet perform this revision-level aggregation.

## Import ARM build contexts

Capture actual compiler invocations in each configuration and convert them to
JSONL. Each record has this shape:

```json
{"configuration":"arm-debug","build_variant":{"product":"device-a","target":"cortex-m4","configuration":"debug"},"source_revision":"<commit>","translation_unit":"src/device.cpp","toolchain":"armclang6","arguments":["--cpu=Cortex-M4","-I","include","-DDEVICE=1"],"dependencies":["include/device.hpp"]}
```

`dependencies` (or `project_files`) is optional, but is needed to map a header
query to the TUs that include it. Import the transient capture into the local
catalog:

```json
{"schema_version":1,"query":"build.import","params":{"input":"C:/capture/build.jsonl"}}
```

The armcc 5/armclang 6 adapter keeps semantic frontend flags, translates common
CPU/FPU/language flags, removes output, warning, debug, and optimization flags,
and reports unsupported ABI options as coverage gaps. Raw captures are not
shared artifacts.

For the end-to-end v1 experiment, use `experiment capture`, `experiment head`,
and `experiment pilot`. These commands intercept compiler calls made by trusted
Make builds, measure extraction coverage, and optionally compare element-level
stability with a developer file partition. See
[`docs/v1-experiment.md`](docs/v1-experiment.md).
Use [`docs/poc-runbook.md`](docs/poc-runbook.md) for the staged PoC procedure,
measurements, and expansion gates.

`experiment pilot` is budgeted progressive analysis. Git screens the full selected
history, Tree-sitter maps changed regions to provisional C/C++ sites directly from
blobs, and only promoted paths reach Make capture and Clang. Required caps are declared
under `pilot.budget`; exhausted caps preserve partial facts but prohibit a definitive
classifier result. Exact repeated screening plans reuse their v1 artifact. Capture,
head, and pilot commands print compact JSON summaries by default; `--full-output` emits
the complete persisted report.

The first PoC assumes a Clang-compatible build. Capture-only operation without a real
compiler remains partial, and ARMCC/ARMCLANG qualification is deferred until the real
vendor build environment and compatibility layer are available.

Build records preserve an explicit `(product, target, configuration)` variant. Its ID is
derived from that tuple; equivalent semantic contexts can still share cached artifacts
without losing their variant observations. The pilot classifier remains scoped to a
build-variant/TU series and does not yet produce one repository-wide cross-TU conclusion.

## Plan Git history

`history.plan` performs a metadata-only first-parent traversal. It does not check
out revisions or invoke Clang. The output is JSONL so later stages can consume a
ten-year plan incrementally.

```json
{
  "schema_version": 1,
  "query": "history.plan",
  "params": {
    "repository": "C:/source/product",
    "ref": "main",
    "output": "C:/artifacts/history-plan.jsonl",
    "pr_facts": "C:/artifacts/bitbucket-pr-facts.jsonl"
  }
}
```

`pr_facts` is optional. A positive JSONL record contains `pr_id` and at least one
of `result_commit`, `merge_result_commit`, or `associated_commits`. A confirmed
direct commit is recorded as
`{"commit":"...","pr_mapping_status":"no_pr"}`. Consecutive first-parent
commits mapped to the same unambiguous PR are emitted as one change unit. The
planner distinguishes `identified`, `ambiguous`, `no_pr`, and `unknown`; an
uncovered commit is never assumed to have bypassed PR review.

Set `start_exclusive` to the last completed commit to create the next partial
plan after an interrupted or previously completed run. The selected range is
still validated against the ref by Git.

## Queries

- `file.history`: return cached file/change facts and schedule missing compiler
  facts. It defaults to direct changes in the previous calendar year across all
  imported configurations.
- `build.import`: import captured ARM build contexts and header dependency maps.
- `history.plan`: write a first-parent commit/change-unit plan without checkouts.
- `lineage.transition`: find automatic continuity candidates and transition facts.
- `lineage.resolve`: apply accepted `same_element` and `not_same_element` assertions.
- `element.history_stats`: summarize ordered snapshots without stability inference.
- `element.explain`: explain one result from a v1 stability report using its historical
  facts, score components, policy, and evidence gaps.
- `analysis.coverage`: return extraction capabilities and gaps.

Requests and responses are one-shot JSON. See
[`docs/lineage-architecture.md`](docs/lineage-architecture.md) for the resource contract.

## Federated local service

Each analysis VM can run a local service while using a Git repository as the
shared fact and coordination transport. VMs never connect to one another.

```json
{
  "schema_version": 1,
  "repository_id": "product-main",
  "catalog": "C:/repotraverse/local-catalog",
  "artifact_repository": "C:/repotraverse/artifacts",
  "remote": "origin",
  "listen_address": "127.0.0.1",
  "port": 7341,
  "sync_seconds": 30,
  "lease_seconds": 900,
  "grace_seconds": 120,
  "worker_concurrency": 2,
  "max_task_attempts": 10,
  "git_timeout_seconds": 300,
  "extractor_timeout_seconds": 1800,
  "max_manifest_bytes": 268435456,
  "workspace_mode": "auto",
  "workspace_max_revisions": 2,
  "workspace_max_bytes": 10737418240,
  "workspace_free_space_reserve_bytes": 5368709120,
  "trusted_producers": [],
  "extractor": "C:/repotraverse/bin/clang-extractor.exe",
  "scratch_root": "C:/repotraverse/worktrees",
  "source_repository": "C:/source/product"
}
```

```powershell
repotraverse serve --config C:\repotraverse\service.json
repotraverse status --endpoint http://127.0.0.1:7341
repotraverse query --endpoint http://127.0.0.1:7341 --request request.json
```

The service creates its producer identity on first startup; do not include the
local catalog directory in a VM image. SQLite is a disposable materialized
index and is never committed. Immutable results use producer branches, while
expiring claims use `repotraverse/claims/<prefix>/<task-id>` branches. The
service worker owns task acquisition, heartbeat, disk-bounded revision workspace
reuse, extraction, and publication. A complete dependency closure uses an exact-path
sparse checkout. Missing closure evidence uses a temporary full checkout and removes it
when its final lease ends.

An LLM normally issues only a high-level query:

```json
{
  "schema_version": 1,
  "query": "file.history",
  "params": {
    "repository": "C:/source/product",
    "ref": "main",
    "path": "source/device.cpp",
    "scope": "direct",
    "pr_facts": "C:/artifacts/bitbucket-pr-facts.jsonl",
    "page_size": 100
  }
}
```

Remote queries create durable request jobs. `POST /v1/requests` queues planning
and immediately returns a stable request ID, `GET /v1/requests/<id>` reads
status without running planning on the HTTP thread, and
`GET /v1/requests/<id>/results` returns the current result. Reposting the same
request resumes a partial or interrupted plan under the same ID. Jobs and
review decisions survive process restarts. The service rejects non-loopback
bind addresses and bounds request size and idle time.

The first job result can be `partial`: it includes Git/PR change units,
zero-context changed-line ranges, the materialized `snapshot_id`, coverage gaps,
and pending work. Repeating the same query after workers finish returns
compiler-derived element snapshots and per-change-unit semantic transition
facts. Pagination uses the returned continuation offset. No full project
snapshot is created.

Low-level `work.*` and `catalog.sync` queries exist for diagnostics and service
coordination. `work.complete` accepts only a schema-v1 `tu_manifest` or
`tu_failure`.

The artifact Git server must allow branch creation and deletion plus
compare-and-swap force pushes below `repotraverse/claims/`. A VM does not start
an expensive task when the remote is unavailable.

Although history is ordered, extraction is parallel: VMs may claim any
revision/TU/context endpoint. Equivalent named configurations share one task;
different semantic contexts remain distinct. A cheap ordered reduction runs
from compact manifests when both endpoints are available.

## Current implementation status

The implemented slice covers project-owned functions and methods, declaration and
definition sites, records,
enums and enumerators, aliases, fields, function and record templates,
specializations, and project macro definitions; external and TU-scoped internal
identity, binding-normalized bodies, semantic variants, dependency fingerprints,
conservative lineage, reviewed lineage assertions in the query API,
first-parent/PR change units, file rename tracking, changed-line ranges, ARM
context import, header-to-TU maps when dependencies are supplied, parallel
automatic extraction, endpoint aggregation in `file.history`, local HTTP queries,
SQLite materialization, Git leases, and immutable producer results.

The experimental stability path currently builds series per explicit build variant and
TU. It does not yet aggregate a logical element across TUs, consume reviewed lineage
assertions, or represent an expected-but-missing target/context observation. Its
classifications are suitable for bounded trials, not a repository-wide multitarget
conclusion. The report retains historical facts separately, including semantic changes,
file Git touches, lifetime revisions, lineage gaps, and provisional policy scores.
Cross-variant semantic divergence is emitted as a separate fact and is not folded into
the temporal classifier.

The implemented observation model is:

```text
logical source element
  revision
    build variant (product, target, configuration)
      semantic variant(s)
        supporting compiler-context and TU observations
```

Logical elements and content-addressed semantic variants will be stored once. TUs will
remain extraction provenance except for genuinely TU-local entities. Temporal evolution,
cross-target divergence, and intra-variant context divergence will remain separate
reported dimensions rather than being flattened into one score.

Semantic scope expands through captured header-to-TU dependency maps and reports
partial coverage when a build context lacks that map. Textual header include-fanout
screening and upstream exposure remain separate from direct element changes; a dependent
change is confirmed only by semantic fingerprints under the same context. Extract and inline changes
are emitted as review-required relations; they never join historical lineage
automatically. Submodules use separate repository identities and explicit
parent-revision to child-revision mappings.

## Production-oriented service operation

Configuration uses schema v1 and requires an explicit
`repository_id`. Non-v1 catalogs and artifacts are intentionally unsupported;
remove and rebuild an old prototype catalog.
Use `config/service.example.json` as the configuration template. On Windows,
the unsigned ZIP can be registered with the Service Control Manager using
`tools/install-service.ps1` and removed using `tools/uninstall-service.ps1`.
The service exposes loopback-only `/v1/status` and `/v1/metrics` diagnostics.
Tasks use bounded child processes, adaptive lease heartbeats, exponential retry,
and quarantine after the configured attempt limit. When `otlp_endpoint` is set,
redacted logs and cumulative metrics are exported asynchronously using OTLP/HTTP
JSON over HTTPS; exporter failure never blocks analysis.

These controls harden deployment and coordination; they do not make the provisional
multirevision classifier analytically complete. Promotion of historical conclusions
requires the element/build-variant aggregation milestone and validation in the real
build environment.

## License

Repotraverse is licensed under the [BSD 2-Clause License](LICENSE).
Third-party components retain their respective licenses as documented in
[THIRD_PARTY.md](THIRD_PARTY.md).
