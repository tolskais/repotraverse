# LLM-guided stability investigation workflow

## Purpose and responsibility boundary

RepoTraverse supplies compiler-, Git-, and integration-history evidence. It does
not label code stable or unstable. An LLM uses the evidence to make a qualified
assessment and must preserve the scope and uncertainty of that evidence.

A stability statement is incomplete unless it names:

- the repository range and branch;
- the build configurations and translation units observed;
- whether it concerns interface, implementation, dependencies, or location;
- the PR/change-unit opportunities actually observed; and
- any coverage or lineage gaps.

For example, "stable" is too broad. A useful conclusion is: "The public
interface of `Parser::parse` is a strong stable candidate across 38 observed PR
change units on the Linux release configuration; its implementation changed in
two of those units, and Windows coverage is unavailable."

## Units of analysis

The primary unit of developer work is an integration unit, normally a merged
PR. RepoTraverse compares its base and result revisions and counts a change
dimension at most once per element and change unit. The commits inside the unit
remain diagnostic evidence for iteration, fixups, reversions, and attribution,
but are not counted as independent developer jobs.

The history planner distinguishes confirmed PRs, direct commits confirmed to
have no PR, compound associations, ambiguous associations, and unknown
associations. An LLM must not silently turn an unknown mapping into a PR.

Semantic history requests contain an ordered endpoint chain:

```text
base of unit 1 -> result of unit 1/base of unit 2 -> result of unit 2 -> ...
```

The `change_units` array must contain one planner integration-unit record per
adjacent bundle pair. Non-contiguous units should be investigated in separate
requests so an unrepresented history gap is not treated as a developer change
unit.

## Expected investigation loop

### 1. Define the question

Before requesting evidence, the LLM defines:

- branch and time or revision range;
- relevant products, targets, and configurations;
- whether the concern is API compatibility, implementation maturity,
  dependency structure, architectural location, or several dimensions; and
- an analysis budget for files, change units, and detailed elements.

The scope can be revised after coverage evidence arrives, but it must not remain
implicit.

### 2. Obtain repository and PR history

Use `history-summary` for a compact overview and `repository-changes` or
`change-unit` for integration-unit details. This stage supplies inexpensive Git
facts such as:

- PR/change-unit identities and association status;
- base, result, and constituent commits;
- changed and renamed files;
- source, build, and documentation change units;
- file touches per integration unit; and
- commit timestamps and subjects.

These facts identify candidate areas, but source touch frequency alone does not
establish semantic change.

### 3. Materialize semantic endpoints

For the configurations in scope, extract compiler evidence at every base/result
endpoint required by the selected contiguous change-unit chain. Preserve the
same configuration and compatible compiler context across a comparison. If an
endpoint, configuration, generated input, or dependency map is unavailable,
record the gap rather than substituting another context.

### 4. Request file-level evidence

Call `file-evidence` with the ordered bundles and aligned planner units:

```json
{
  "bundles": [
    "cache/pr-101-base.json",
    "cache/pr-101-result.json",
    "cache/pr-102-result.json"
  ],
  "change_units": [
    {
      "change_unit_id": "pr-101",
      "base_commit": "a111",
      "result_commit": "b222",
      "association_status": "confirmed_pr",
      "integrated_commits": ["b201", "b222"]
    },
    {
      "change_unit_id": "pr-102",
      "base_commit": "b222",
      "result_commit": "c333",
      "association_status": "confirmed_pr",
      "integrated_commits": ["c333"]
    }
  ],
  "revision_timestamps": {
    "a111": 1735689600,
    "b222": 1736294400,
    "c333": 1736899200
  }
}
```

The response reports per-file observed and current elements, observed change
units, units containing interface/implementation/dependency changes, moves,
renames, unresolved evidence, changed-element counts, and ordered file events.
It is a semantic profile, not a replacement for Git-level direct file touches.

If `change_units` is omitted, the response sets `grouping_unit` to
`revision_transition`, keeps generic transition counts, and sets PR/change-unit
count fields to `null`. The LLM must not describe those values as PR counts.

### 5. Select the finer analysis scope

Combine Git-level and semantic file facts. Select a bounded set containing:

1. files changed by many independent units or changed recently;
2. files with interface or dependency changes;
3. public headers and high-impact dependency nodes, even when quiet;
4. representative quiet files with complete evidence; and
5. files with partial coverage or unresolved lineage.

This selection is intentionally broader than a hotspot list. Otherwise the LLM
would inspect only active code and bias the repository assessment toward
instability. Low observed activity with poor coverage belongs in the uncertain
set, not the stable set.

The tool may sort or filter by factual dimensions to satisfy a caller-supplied
budget. It must not invent a stability score to choose the files.

### 6. Request element-level evidence

Call `element-evidence` for the selected scope using the same ordered bundles
and change units. For each resolved historical element, the response includes:

- current name, kind, path, and latest presence;
- exact observed revisions and optional timestamps;
- adjacent observable transitions and observed change units;
- units with interface, implementation, and dependency-set changes;
- moves, renames, ambiguity, and unresolved transitions;
- the last observed and last content-change revisions;
- ordered change events with change-unit identity and confidence; and
- element-level and overall coverage.

An observation gap is not counted as an opportunity. An ambiguous or
unreviewed successor does not silently join two histories.

### 7. Drill into meaningful transitions

Summary counts identify where to ask the next question; they rarely explain why
a change occurred. For selected events, use `lineage.transition`,
`symbol-relations`, `change-unit`, file/source diffs, and connector evidence to
obtain:

- before and after locations and identities;
- exact continuity and lineage confidence;
- direct and reverse semantic relations;
- PR title, description, issue, and review evidence when available;
- constituent commit trajectory; and
- build-context or generated-input changes.

Dependency change currently means that the directly referenced declaration set
changed. It does not mean that an unchanged dependency identity changed its own
implementation. The latter must be investigated through that dependency's
history.

### 8. Produce a qualified assessment

Assess interface, implementation, dependency, and location stability
separately. Use at least these outcomes:

- **strong stable candidate**: substantial continuous observation, complete
  evidence, and little or no change in the stated dimension;
- **evolving candidate**: repeated changes across independent units, especially
  recent interface or dependency changes; and
- **insufficient evidence**: too few opportunities, incomplete configurations,
  observation gaps, or unresolved lineage.

Thresholds belong to the LLM's reviewed analysis policy and should reflect the
repository's integration rate and the element's role. Ten observed units may be
substantial in a quiet repository and weak evidence in a repository merging
hundreds of PRs per month.

## Interpreting the dimensions

An element does not have one indivisible stability property:

| Evidence pattern | Appropriate interpretation |
| --- | --- |
| No interface changes; several body changes | Interface stable-looking, implementation evolving |
| No body changes; repeated dependency changes | Implementation shape quiet, dependency structure evolving |
| No recorded changes; incomplete coverage | Insufficient evidence |
| Repeated changes in one PR's commits; one net PR change | One developer change with substantial internal iteration |
| No direct file touch; compiler fingerprint changed | Possible induced or context-dependent semantic change |

Recency, architectural impact, and independent change-unit count are generally
more informative than raw commit count. A public high-fan-in declaration also
deserves more scrutiny than a private leaf helper with the same history.

## Expected LLM report

The final report should preserve evidence and inference as separate sections:

```json
{
  "scope": {
    "branch": "main",
    "from": "2024-01-01",
    "to": "2026-01-01",
    "configurations": ["linux-release"]
  },
  "evidence_summary": {
    "integration_units": 84,
    "files_profiled": 126,
    "files_selected_for_element_analysis": 14,
    "coverage_status": "partial"
  },
  "assessments": [
    {
      "subject": "Parser::parse",
      "dimension": "interface",
      "assessment": "strong_stable_candidate",
      "observed_change_units": 38,
      "change_units_with_changes": 0,
      "confidence": "high",
      "limitations": ["Windows configuration unavailable"]
    }
  ]
}
```

The `assessment` fields are LLM inference and must not be copied into
RepoTraverse fact records. Reports should cite the relevant transition IDs,
change-unit IDs, and coverage gaps so another investigator can reproduce or
challenge the conclusion.

## Stop and expansion conditions

The LLM may stop refining a subject when the requested dimensions have adequate
continuous coverage, lineage is resolved, and additional endpoints are unlikely
to change the qualified conclusion. It should expand the investigation when:

- a high-impact file has partial evidence;
- change-unit association is ambiguous;
- a configuration produces a different semantic variant;
- a dependency change reaches important callers;
- apparent stability depends on a short or old observation window; or
- file-level and element-level evidence disagree.

The result is therefore an auditable investigation, not a permanent label. A
new PR, configuration, or reviewed lineage assertion can change the evidence and
justify a new assessment.
