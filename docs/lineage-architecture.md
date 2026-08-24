# Lineage architecture

## Responsibility boundary

The factual core reports compiler-observed source elements, conservative continuity,
coarse content changes, source locations, coverage, and historical counts. It does not
claim exhaustive coverage or infer developer intent. An LLM or developer may review
lineage and store explanations separately. The optional v1 experiment applies an
explicit policy to those facts to produce provisional `stable`, `variable`, or
`insufficient_evidence` labels; those labels are not factual core records.

## Snapshot facts

Each element snapshot contains compiler identity, kind, qualified name, source location,
and three opaque fingerprints:

- interface: canonical type and declaration shape, excluding name and location;
- implementation: AST shape with local and parameter bindings canonicalized;
- dependency: the sorted resolved declaration identities referenced by the body.

Raw source and raw AST bodies are not persisted.

Schema v1 separates a repository, build target, normalized compile context, logical
element, semantic variant, element observation, and TU manifest. External
logical elements are shared across TUs; internal-linkage elements include the
TU in their identity. `file.history` unions endpoint observations and variants while
retaining the build variants, configurations, contexts, and TUs that support each fact.
The current experimental pilot still reduces build-variant/TU series independently;
revision-level cross-TU aggregation remains to be implemented.

The intended history model stores a logical source element once, then indexes its
observations by revision and explicit build variant. A build variant includes product,
target, and configuration labels; its normalized semantic contexts retain the effective
target triple, CPU, ABI, language mode, defines, include paths, preincludes, sysroot,
toolchain profile, and feature flags. Equivalent TU observations support one semantic
variant rather than creating duplicate elements. Different variants are retained and
reported, never flattened merely because they share a source location.

An element can therefore be temporally stable within each target while intentionally
different across targets. Temporal evolution, cross-build-variant divergence, and
intra-variant context divergence are separate facts. Successful semantic absence,
unobserved context, and failed extraction are also separate states.

## History plans

`history.plan` writes a JSONL resource from a selected ref's first-parent history. A
header records the repository and traversal parameters, each `change_unit` records
commits, trees, subjects, changed paths, Git rename scores, and optional Bitbucket PR
matches, and a footer records factual totals. The planning pass reads Git metadata only:
it does not check out trees or run the compiler.

PR associations are imported facts. Consecutive commits are grouped only when both have
the same single PR match. A zero-result Bitbucket lookup is persisted as `no_pr`, while a
commit absent from the export remains `unknown`; multiple associations are `ambiguous`.
Author or timestamp proximity is never used to infer grouping.

`file.history` follows the requested path backward through Git renames and
returns only a bounded page. Each change unit includes zero-context old/new line
ranges, not changed source text. Its default window is the previous calendar
year. Missing endpoint manifests become independent revision/TU/context tasks;
the query remains usable and explicitly partial while they run.

## Transition facts

Continuity is one of `same`, `renamed`, `moved`, `moved_and_renamed`,
`added_or_unresolved`, or `deleted_or_unresolved`. Content change is `none`, `interface`,
`implementation`, `both`, or `unverified`. Relocation and content are independent, so a
pure move never becomes implementation churn.

Matching proceeds conservatively: exact compiler identity first, then a unique exact
interface/implementation/dependency shape. Multiple exact candidates remain ambiguous.
In the current transition implementation, a unique exact-shape candidate is resolved
automatically with `high` confidence; multiple matches are review-only. False merges are
considered worse than missed refactorings.

## Managed assertions

Assertions have before/after element identities, relation, author, review status, and
reviewer. Only `accepted` assertions affect resolution. `same_element` forces a lineage
edge; `not_same_element` vetoes an automatic shape candidate. Proposed assertions remain
managed knowledge but do not alter facts.

## History statistics

`element.history_stats` unions resolved identities across explicitly ordered bundles
and reports observed versions, observable transitions,
content/interface/implementation/dependency changes, moves, renames, ambiguities,
lifetime revisions, and the last content-change revision. Stability reports add
revision-level owning-file Git touches when the pilot supplies them. The progressive
screening artifact separately joins changed ranges to provisional syntax sites; those
touches do not become canonical element changes until Clang confirms the promoted
candidate. These are factual
inputs for inference, not stability conclusions. The separate experiment classifier
adds a documented weighting and threshold policy. It does not yet consume managed
lineage assertions or normalized multitarget element states.

## Progressive history analysis

The public pilot does not send every file at every revision directly to Clang. Git first
computes repository-wide file facts. Explicit per-stratum budgets select COMMON leakage,
VARIABLE detail, stable-island candidates, high-impact headers, and deterministic control
samples. Tree-sitter then parses selected C/C++ blobs and maps each hunk against the
ranges from both historical endpoints. HEAD ranges are never projected backward.

Tree-sitter sites have a `syntactic_candidate` identity kind. Names, enclosing syntax,
interface shape, and structural shape may propose work or lineage, but cannot establish
canonical identity or a stability label. Clang confirms promoted candidates in every
available target/configuration observation. Git and syntax stages require neither a
worktree nor a compile command.

Element change facts separate intrinsic source/declaration changes from upstream
exposure, confirmed induced semantic changes, and build-configuration changes. Include
edges alone never increment direct element changes. Declaration and definition locations
are observations of one Clang logical element. Bounded reverse dependency traversal
retains causal element/path sets; exhausted budgets become coverage gaps.

## Federated catalogs and leases

Every VM serves queries from a transactionally consistent local SQLite index. The
index is derived and can be rebuilt from immutable JSONL result commits fetched from
the artifact Git repository. Each producer writes only
`repotraverse/producers/<producer-id>`; a producer ID is generated after VM deployment.

Compiler extraction task IDs cover the source commit, translation unit,
normalized configuration context, extractor identity, and schema version.
Task batches are immutable commits under
`repotraverse/tasks/<producer-id>`, so every VM discovers the same work without
connecting to another VM. Before extraction, a VM must atomically create the
corresponding `repotraverse/claims/` ref. Claims expire and are renewed or taken
over using Git compare-and-swap updates. The result is pushed before the claim
is marked complete, so a VM crash cannot hide a published fact.

Each VM configures its own source-repository path; paths embedded by the scheduling VM
are informational. Tasks carry a materialization manifest. A complete dependency
closure uses a shared exact-path sparse revision workspace; the required path set grows
monotonically as TUs arrive. Incomplete closure evidence upgrades the revision to a
temporary full workspace. Byte/count limits, a free-space reserve, leases, and LRU
eviction bound local disk use. A failed frontend run publishes a typed failure with a
diagnostics fingerprint rather than compiler output.

The local HTTP response may be partial. It identifies the materialized snapshot,
observed coverage, and pending task ownership. Git synchronizes facts and leases;
VMs do not expose services to one another.

History ordering does not serialize compiler work. Snapshot extraction is
independent for each revision, translation unit, and normalized configuration
context, so VMs claim those tasks in parallel. The query performs a cheap
ordered reduction after both endpoints arrive. Exact logical identity produces
facts; a unique exact semantic-shape match currently produces a high-confidence
automatic fact and candidate, while ambiguous matches remain reviewable. The LLM or
developer can record accepted or rejected lineage separately.

## Production-oriented service boundaries

Every logical identifier includes an explicit repository ID. Submodules retain
independent histories; a mapping records which child revision is pinned by a
parent revision. Cross-repository lineage is never inferred.

HTTP is loopback-only and exposes durable request jobs rather than holding a
connection for compiler work. Git is authoritative for shared facts, reviews,
tasks, and leases. The versioned SQLite catalog provides a local transactional
queue, durable request state, and indexes that can be rebuilt from validated
schema-v1 Git artifacts. Imported compile contexts are local operational input
and must be reimported after catalog replacement. Imported producer records are
size-bounded, schema checked, content-address verified, and optionally restricted
by producer ID.

Functions, methods, records, enums, enumerators, aliases, fields, templates,
specializations, and macro definitions are logical elements. Extract and inline
relationships are non-bijective candidate relations and require an accepted
review before affecting lineage.
