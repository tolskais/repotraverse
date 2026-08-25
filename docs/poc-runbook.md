# Proof-of-concept runbook

This runbook validates whether RepoTraverse produces useful and auditable historical
element evidence on one real repository. It does not qualify stability conclusions,
prove ten-year scale, or qualify ARMCC/ARMCLANG compatibility.

## Pre-PoC validation evidence

As of 2026-08-23, the implementation passed all 21 repository tests. Bounded external
checks also established the following capability boundaries:

- U-Boot v2021.04 `common/main.c` produced 8,509 elements without the prior anonymous-
  declaration manifest collision. Its reused, older capture remained partial because it
  predated GCC dependency-output recognition.
- An Mbed OS 5.15.9 Git/Tree-sitter screening over 20 first-parent revisions examined
  16,945 source/header files and promoted 28 provisional elements. On that host, a cold
  run took 44.3 seconds and an identical cached run 6.3 seconds; the compact pilot report
  was 76 KiB while the detailed screening artifact remained 8 MiB. Semantic capture was
  deliberately capped at zero, so the result was partial and produced no classification
  evidence.

These are regression checks, not target-repository accuracy or capacity claims.

## Supported PoC profiles

Choose one profile and record it with the results:

1. **Clang pass-through.** The trusted Make target emits Clang-compatible arguments and
   `real_compiler` names the compiler. This is the preferred profile because real object
   and dependency files can be observed.
2. **Capture-only.** Omit `real_compiler`. The probe materializes expected output files so
   Make can continue, but it cannot synthesize a real dependency map. Semantic extraction
   may still work after argument normalization; coverage remains partial and content-
   addressed TU reuse is disabled.
3. **ARM vendor qualification (deferred).** Use only when the real ARMCC/ARMCLANG build
   environment and compatibility layer are available. Do not describe a Clang-only run as
   ARM toolchain qualification.

The probe invokes one `real_compiler` executable for captured C and C++ sources. Select a
driver that chooses its language from the source extension, or use separate experiment
configurations when the build requires distinct drivers.

## Before running

- Build with the Clang extractor enabled and run the complete test suite.
- Commit or otherwise record the RepoTraverse revision used for the PoC.
- Ensure the target repository is checked out at the revision named by the manifest.
  Capture and HEAD reject a different checkout.
- Use trusted build commands only. Experiment commands execute repository build logic.
- Create one configuration for one product, target, and build configuration first.
- Give capture, HEAD, and pilot separate output directories. Capture and HEAD require a
  new directory. Resume a pilot only with the identical repository range, partition,
  budget, build configuration, and policy; use a new directory after changing any of
  them.
- Place the output and artifact cache on storage with enough headroom for one temporary
  full revision workspace. The first captured pilot revision and every incomplete
  dependency closure require a temporary full workspace.

Start from [`config/experiment.example.json`](../config/experiment.example.json). Replace
all paths, labels, build commands, partition globs, and the compiler path. The policy in
that file is a baseline hypothesis for observation, not a calibrated product definition.

## Stage 1: HEAD semantic smoke test

Use a dedicated manifest with a new `output`, the repository's checked-out HEAD, and one
configuration:

```text
repotraverse experiment head --manifest poc-head.v1.json
```

The command prints a compact summary. Preserve `head-report.v1.json` and inspect:

- `capture.import.records` and `translation_unit_contexts` are nonzero;
- `capture.import.ignored_non_translation_unit_records` is plausible for compiler checks
  and link steps;
- every failed unit has a typed `failure` and useful `failure_detail`;
- complete, partial, and failed context counts are recorded;
- dependency-map coverage, extracted element count, elapsed/CPU time, host-observed peak
  memory, and artifact bytes are recorded; and
- repeated logical element IDs do not invalidate a TU manifest.

Do not require complete dependency coverage from a capture-only run. Instead, record that
the run cannot validate sparse reuse, header-directed invalidation, or the semantic cache.
Resolve unexplained extraction failures before adding historical revisions.

## Stage 2: bounded progressive pilot

Use a separate pilot output. Begin with the most recent 20 first-parent revisions and the
conservative per-stratum caps in the example manifest:

```text
repotraverse experiment pilot --manifest poc-pilot.v1.json
```

Preserve both `progressive-screening.v1.json` and `pilot-report.v1.json`. Record:

- number of screened source/header files;
- selected paths, syntax transitions, promoted elements, and semantic revisions;
- complete/partial/failed TU contexts by revision;
- exact screening-plan and syntax-blob cache hits;
- lineage exact/high-confidence/ambiguous/unresolved counts when available;
- each `evidence_gap.kind`, especially history, syntax-budget, dependency, extraction, and
  lineage gaps;
- wall/CPU time, host-observed peak memory, workspace peak bytes, and artifact sizes; and
- stable/variable/insufficient-evidence counts, without tuning the policy to improve
  apparent agreement.

If `max_capture_revisions` is smaller than the selected history, the resulting semantic
history is intentionally incomplete and cannot support a definitive classification.
Increase only after the bounded run is operationally understood.

## Stage 3: human usefulness review

Review the highest-ranked 20–50 COMMON leakage and VARIABLE stable-island candidates with
developers who know the architecture. For each candidate, distinguish:

- useful localization of intrinsic variability;
- context or dependency sensitivity rather than direct source variability;
- lineage or extraction coverage failure;
- partition mismatch; and
- disagreements between reviewed interpretations and the reported facts.

Weights and thresholds remain unchanged during the first review. The PoC succeeds when
the factual evidence is reproducible, explainable, and useful enough to justify a larger
history window—not when a particular stable/variable agreement percentage is reached.

## Expansion gates

Expand from 20 revisions to a longer window only when:

- the HEAD and pilot runs have no unexplained systematic extraction failure;
- coverage gaps are quantified by cause;
- incremental reuse produces the same manifests as a full analysis on synthetic tests;
- disk, memory, runtime, and artifact growth are measured and acceptable;
- ambiguous/unresolved lineage is measured rather than silently joined; and
- developers find actionable variation leakage or stable-island evidence.

ARMCC/ARMCLANG qualification, cross-TU revision aggregation, expected target/context
absence and a 500K/ten-year capacity claim remain later gates.
