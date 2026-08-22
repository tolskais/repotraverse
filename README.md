# Element History Analyzer

This prototype constructs C/C++ element lineage across Git revisions. Git remains the
source of exact changed code; artifacts contain compact compiler-derived fingerprints,
locations, lineage candidates, transition facts, and factual history statistics.

## Windows VM build

Windows builds use `clang-cl` from a prebuilt LLVM/Clang SDK. Run the build from
an initialized x64 Visual Studio developer environment so the Windows SDK,
linker, and runtime are available. CMake and Ninja must also be on `PATH`.

The SDK must contain:

```text
<llvm-root>/bin/clang-cl.exe
<llvm-root>/lib/cmake/llvm/LLVMConfig.cmake
<llvm-root>/lib/cmake/clang/ClangConfig.cmake
```

The CMake packages must export the monolithic `clang-cpp` and `LLVM` library
targets. This keeps the VM build independent of LLVM's internal component list.

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

The build is offline: nlohmann/json v3.12.0 is vendored and CMake contains no
dependency download path. Its MIT license is retained under
`third_party/nlohmann_json/LICENSE.MIT` and installed with the tool.

The core-only preset still uses clang-cl as the compiler but does not link the
Clang libraries:

```powershell
.\tools\build-windows.ps1 -Mode CoreOnly -LlvmRoot C:\llvm-sdk
```

## Developer build

Non-Windows developer machines can select an installed Clang package explicitly:

```sh
cmake -S . -B build -G Ninja \
  -DREPOTRAVERSE_CLANG_PROVIDER=package \
  -DLLVM_DIR=/usr/lib/llvm/22/lib64/cmake/llvm \
  -DClang_DIR=/usr/lib/llvm/22/lib64/cmake/clang
cmake --build build
ctest --test-dir build --output-on-failure
```

The core can be built without linking LLVM using
`-DREPOTRAVERSE_CLANG_PROVIDER=disabled`.

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
AST edit scripts. They do record the Repotraverse version, LLVM version, Clang
version, build mode, and host architecture so facts produced by different
extractors remain identifiable.

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
