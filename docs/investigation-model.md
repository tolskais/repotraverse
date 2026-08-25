# Repository investigation model

Repotraverse reports evidence. It does not classify code as stable or unstable,
produce a stability score, or rank architectural changes. A caller can combine
the reported facts with reviewed inference to make those decisions.

## Storage boundary

The source repository remains authoritative for commits, trees, diffs, paths,
authors, and timestamps. Repotraverse computes those inexpensive facts on
demand and does not copy them into the analysis repository.

The analysis repository stores only information that is expensive to recreate
or useful to review:

- PR association imports and their provenance;
- inference proposals, review decisions, and taxonomy;
- compact evidence receipts containing identities, fingerprints, coverage, and
  result digests; and
- temporary task, lease, and extraction-result refs used for coordination.

Full compiler manifests, build inventories, dependency closures, SDK files, and
generated inputs are local cache entries. Temporary result refs are one commit
per task and are removed after the completion lease expires. Git object garbage
collection may reclaim the unreachable payload later.

All refs use `repotraverse/v1/`. Review decisions are authoritative only when
they are reachable from the configured `knowledge_ref`.

## Identities and evidence

An integration unit has an immutable ID derived from its base, result, ordered
integrated commits, and PR association evidence. Consecutive first-parent
commits associated with the same PR form one unit; disjoint spans form separate
units. Commits confirmed to have no PR are individual units. Unknown and
ambiguous associations are reported rather than guessed. Changes are the net
base-to-result diff, including empty net effects.

Compiler observations, logical elements, and transitions have separate
immutable IDs. Extraction identities include the build inventory, normalized
compile context, toolchain/SDK, generated-input, and extractor fingerprints.
Generated or non-Git inputs are represented by digests.

Origin is non-exclusive evidence. A transition can have direct Git evidence,
dependency-induced evidence, build-context evidence, and unresolved evidence
at the same time. Header inclusion proves translation-unit exposure only;
element exposure requires a compiler relation, and induced change requires an
aligned semantic change.

Header investigations use the union of known including translation units at
both endpoints. Coverage is complete only when the declared inventories and
dependency maps are complete and every expected endpoint/context-family cell is
observed.

## Raw tool operations

Use `repotraverse tool OPERATION --endpoint URL --input request.json`. The
input is an operation-specific JSON object. Responses keep `facts` and
`inference` separate.

The initial operations are:

- `pr-import`, `repository-changes`, `history-summary`, and `change-unit`;
- `file-symbols`, `symbol-search`, `symbol-history`, and `symbol-relations`;
- `receipt-put`, `receipt-get`, `claim-propose`, `claim-verify`, and
  `inference-get`; and
- `request-status`.

Results are bounded and report truncation or continuation. MCP can later wrap
this surface without changing the evidence model.
