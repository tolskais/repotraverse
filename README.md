# Element History Analyzer

This prototype constructs C/C++ element lineage across Git revisions. Git remains the
source of exact changed code; artifacts contain compact compiler-derived fingerprints,
locations, lineage candidates, transition facts, and factual history statistics.

## Build

```sh
cmake -S . -B build -G Ninja \
  -DANALYSIS_CLANG_PROVIDER=package \
  -DLLVM_DIR=/usr/lib/llvm/22/lib64/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm/22/lib64/cmake/clang
cmake --build build
ctest --test-dir build --output-on-failure
```

The core can be built without LLVM using `-DANALYSIS_CLANG_PROVIDER=disabled`.

## Extract snapshots

Run `clang-extractor` once for a translation unit under each analyzed revision and build
configuration. A compilation database may be supplied with `-p`.

```sh
clang-extractor -p build \
  --source-revision <revision> \
  --configuration <configuration> \
  --context-fingerprint <compile-command-hash> \
  source/device.cpp > device.json
```

Persistent snapshots do not contain function bodies, expressions, source snippets, or
AST edit scripts.

## Queries

- `lineage.transition`: find automatic continuity candidates and transition facts.
- `lineage.resolve`: apply accepted `same_element` and `not_same_element` assertions.
- `element.history_stats`: summarize ordered snapshots without stability inference.
- `analysis.coverage`: return extraction capabilities and gaps.

Requests and responses are one-shot JSON. See
[`docs/lineage-architecture.md`](docs/lineage-architecture.md) for the resource contract.

## Current scope

The implemented vertical slice covers project-owned functions and methods, exact compiler
identity, unique exact-shape move/rename recognition, binding-normalized local/parameter
renames, ambiguity preservation, reviewed assertions, and multi-revision factual counts.
Types, fields, templates, macros, artifact catalogs, incremental planning, and advanced
extract/inline refactorings are subsequent milestones.
