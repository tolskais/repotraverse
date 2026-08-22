# Third-party components

The production package contains these pinned source dependencies:

| Component | Version | License |
| --- | --- | --- |
| nlohmann/json | 3.12.0 | MIT |
| SQLite | 3.53.4 | Public domain |
| xxHash | 0.8.3 | BSD 2-Clause |

The Clang-enabled executable is linked against the LLVM/Clang SDK supplied by
the internal build environment. Its exact LLVM and Clang versions are embedded
in `clang-extractor --version` and must be added to the release SBOM by the
release pipeline.
