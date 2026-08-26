# Third-party components

Repotraverse itself is licensed under the BSD 2-Clause License; see
[`LICENSE`](LICENSE). The following components retain their upstream licenses.

The source tree contains these pinned dependencies; Catch2 is test-only:

| Component | Version | License |
| --- | --- | --- |
| nlohmann/json | 3.12.0 | MIT |
| SQLite | 3.53.4 | Public domain |
| xxHash | 0.8.3 | BSD 2-Clause |
| Tree-sitter runtime | 0.26.11 | MIT |
| Tree-sitter C grammar | 0.24.2 | MIT |
| Tree-sitter C++ grammar | 0.23.4 | MIT |
| CLI11 | 2.7.0 | BSD 3-Clause |
| Catch2 | 3.15.0 | Boost Software License 1.0 |

Tree-sitter is built from the vendored runtime and generated parser sources.
RepoTraverse does not require the Tree-sitter CLI, a package manager, or a
network connection during configuration or compilation.
CLI11 is built from its official single-header distribution. Catch2 is used
only by test targets and is built from its official amalgamated source
distribution.

The Clang-enabled executable is linked against the LLVM/Clang SDK supplied by
the internal build environment. Its exact LLVM and Clang versions are embedded
in `clang-extractor --version`. Windows packaging records the LLVM project and
version in the release SBOM and installs the SDK's `LICENSE.TXT` under
`licenses/llvm`.

# libcurl

Repotraverse links against the externally provisioned libcurl 8.21.0 SDK.
Windows builds use a static Schannel/SSPI build supplied through
`REPOTRAVERSE_CURL_ROOT`; CMake never downloads it. libcurl is licensed under
the curl license: https://curl.se/docs/copyright.html
