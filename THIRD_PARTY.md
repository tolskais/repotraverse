# Third-party components

Repotraverse itself is licensed under the BSD 2-Clause License; see
[`LICENSE`](LICENSE). The following components retain their upstream licenses.

The v1 package contains these pinned source dependencies:

| Component | Version | License |
| --- | --- | --- |
| nlohmann/json | 3.12.0 | MIT |
| SQLite | 3.53.4 | Public domain |
| xxHash | 0.8.3 | BSD 2-Clause |
| Tree-sitter runtime | 0.26.11 | MIT |
| Tree-sitter C grammar | 0.24.2 | MIT |
| Tree-sitter C++ grammar | 0.23.4 | MIT |

Tree-sitter is built from the vendored runtime and generated parser sources.
RepoTraverse does not require the Tree-sitter CLI, a package manager, or a
network connection during configuration or compilation.

The Clang-enabled executable is linked against the LLVM/Clang SDK supplied by
the internal build environment. Its exact LLVM and Clang versions are embedded
in `clang-extractor --version`. Windows packaging records the LLVM project and
version in the release SBOM and installs the SDK's `LICENSE.TXT` under
`licenses/llvm`.
