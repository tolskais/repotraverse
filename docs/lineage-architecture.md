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
