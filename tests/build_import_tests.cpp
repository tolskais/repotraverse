#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "history/build_import.hpp"
#include "history/catalog.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-build-import-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    std::filesystem::create_directories(root);
    const auto input = root / "build.jsonl";
    std::ofstream output(input);
    for (const auto *configuration : {"debug", "release"})
      output << nlohmann::json(
                    {{"configuration", configuration},
                     {"schema_version", history::kSchemaVersion},
                     {"source_revision", "revision-1"},
                     {"translation_unit", "src/device.cpp"},
                     {"toolchain", "armclang6"},
                     {"arguments", {"-I", "include", "-DDEVICE=1", "-O2"}},
                     {"dependencies", {"include/device.hpp"}}})
                    .dump()
             << '\n';
    output << nlohmann::json({{"configuration", "debug"},
                              {"schema_version", history::kSchemaVersion},
                              {"source_revision", "revision-1"},
                              {"translation_unit", ""},
                              {"invocation_kind", "non_translation_unit"},
                              {"toolchain", "armclang6"},
                              {"arguments", {"-E", "-dM", "-"}}})
                  .dump()
           << '\n';
    output << nlohmann::json(
                  {{"configuration", "legacy"},
                   {"schema_version", history::kSchemaVersion},
                   {"source_revision", "revision-1"},
                   {"translation_unit", "src/device.cpp"},
                   {"toolchain", "armcc5"},
                   {"arguments", {"--cpu=Cortex-M4", "--apcs=/interwork"}},
                   {"project_files", {"include/device.hpp"}}})
                  .dump()
           << '\n';
    output.close();

    history::Catalog catalog(root / "catalog");
    const auto imported = history::import_build_log(catalog, input);
    require(imported.at("records") == 3, "build records missing");
    require(imported.at("ignored_non_translation_unit_records") == 1,
            "compiler capability invocation was not ignored");
    require(imported.at("distinct_contexts") == 2,
            "equivalent configurations were not deduplicated");
    require(imported.at("partial_contexts") == 1,
            "unsupported armcc option was not reported");
    const auto contexts =
        catalog.compile_contexts("include/device.hpp", "revision-1");
    require(contexts.size() == 3,
            "header-to-translation-unit mapping was not indexed");
    const auto &arguments = contexts.front().frontend_arguments;
    require(arguments.size() >= 2 && arguments[0] == "-I" &&
                arguments[1] == "include",
            "separate include argument was not preserved");

    history::Catalog golden_catalog(root / "golden-catalog");
    const auto golden = history::import_build_log(
        golden_catalog,
        std::filesystem::path(TEST_FIXTURE_DIR) / "armcc-capture.v1.jsonl");
    require(golden.at("records") == 2 && golden.at("partial_contexts") == 1,
            "golden ARM capture coverage was not preserved");
    const auto golden_contexts =
        golden_catalog.compile_contexts("src/device.cpp", "0123456789abcdef");
    require(golden_contexts.size() == 2,
            "golden ARM configurations were not imported");
    const auto armcc = std::find_if(
        golden_contexts.begin(), golden_contexts.end(),
        [](const auto &context) { return context.toolchain == "armcc5"; });
    require(armcc != golden_contexts.end() &&
                std::find(armcc->frontend_arguments.begin(),
                          armcc->frontend_arguments.end(), "-mcpu=Cortex-R5") !=
                    armcc->frontend_arguments.end() &&
                armcc->coverage.status == "partial",
            "golden ARMCC translation or APCS gap is missing");
    std::cout << "build import tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
