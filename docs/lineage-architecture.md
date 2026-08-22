# Lineage architecture

## Responsibility boundary

The tool exhaustively traces elements that Git cannot track. It reports continuity,
coarse content changes, source locations, coverage, and historical counts. It does not
infer developer intent or classify code as stable or dynamic. An LLM reads selected Git
changes and may store reviewed explanations separately.

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
TU in their identity. A revision/file view unions observations and variants
while retaining the configurations, contexts, and TUs that support each fact.

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
False merges are considered worse than missed refactorings.

## Managed assertions

Assertions have before/after element identities, relation, author, review status, and
reviewer. Only `accepted` assertions affect resolution. `same_element` forces a lineage
edge; `not_same_element` vetoes an automatic shape candidate. Proposed assertions remain
managed knowledge but do not alter facts.

## History statistics

`element.history_stats` unions resolved identities across ordered bundles and reports
observed versions, observable transitions, content/interface/implementation changes,
moves, renames, ambiguities, and the last content-change revision. These are factual
inputs for LLM inference, not stability conclusions.

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

Each VM configures its own source-repository path; paths embedded by the
scheduling VM are informational. Worktrees contain one checked-out revision and
are deleted after extraction. A failed frontend run publishes a typed failure
with a diagnostics fingerprint rather than compiler output.

The local HTTP response may be partial. It identifies the materialized snapshot,
observed coverage, and pending task ownership. Git synchronizes facts and leases;
VMs do not expose services to one another.

History ordering does not serialize compiler work. Snapshot extraction is
independent for each revision, translation unit, and normalized configuration
context, so VMs claim those tasks in parallel. The query performs a cheap
ordered reduction after both endpoints arrive. Exact logical identity produces
facts; unique semantic-shape matches are emitted only as reviewable lineage
candidates. The LLM or developer records accepted lineage separately.

## Production boundaries

Every logical identifier includes an explicit repository ID. Submodules retain
independent histories; a mapping records which child revision is pinned by a
parent revision. Cross-repository lineage is never inferred.

HTTP is loopback-only and exposes durable request jobs rather than holding a
connection for compiler work. The local catalog persists job and review state,
uses a versioned SQLite schema, and can rebuild its fact index from validated
schema-v1 Git artifacts. Imported producer records are size-bounded, schema
checked, content-address verified, and optionally restricted by producer ID.

Functions, methods, records, enums, enumerators, aliases, fields, templates,
specializations, and macro definitions are logical elements. Extract and inline
relationships are non-bijective candidate relations and require an accepted
review before affecting lineage.
