#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

#include "history/ir.hpp"
#include "history/process.hpp"

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
}

int main() {
  try {
    const auto fixture =
        std::filesystem::path(TEST_FIXTURE_DIR) / "entities.cpp";
    const auto extracted = history::run_process(
        {EXTRACTOR_PATH,
         "--repository-id", "fixture-main",
         "--source-revision", "revision",
         "--source-blob", "blob",
         "--context-fingerprint", "context",
         "--project-root", std::filesystem::path(TEST_FIXTURE_DIR).string(),
         fixture.string(), "--", "-std=c++20"});
    require(extracted.exit_code == 0, "entity extraction failed");
    const auto manifest = nlohmann::json::parse(extracted.output)
                              .get<history::TuManifest>();
    std::string validation_error;
    require(history::validate_tu_manifest(manifest, validation_error),
            "entity manifest failed validation");
    std::set<std::string> kinds;
    for (const auto &element : manifest.elements) kinds.insert(element.kind);
    for (const auto *kind : {"record_template", "field", "method", "enum",
                             "enum_constant", "type_alias", "macro"})
      require(kinds.contains(kind), "expected C++ element kind was not extracted");
    require(!manifest.macro_expansions.empty(),
            "macro expansion dependency was not extracted");
    bool declaration = false, definition = false;
    for (const auto &observation : manifest.observations) {
      declaration = declaration || observation.location.role == "declaration";
      definition = definition || observation.location.role == "definition";
    }
    require(declaration && definition,
            "declaration and definition sites were not preserved separately");
    const auto declared = std::find_if(
        manifest.elements.begin(), manifest.elements.end(), [](const auto &item) {
          return item.qualified_name == "declared" && item.kind == "function";
        });
    require(declared != manifest.elements.end(),
            "golden function was not extracted");
    const auto declared_variant = std::find_if(
        manifest.variants.begin(), manifest.variants.end(),
        [&](const auto &item) { return item.element_id == declared->element_id; });
    require(declared_variant != manifest.variants.end(),
            "golden function variant was not extracted");
    require(declared_variant->implementation_fingerprint ==
                "56446a49e36ec7fa690faa8ef99e9fb2",
            "canonical body fingerprint changed");
    const auto implementation = [&](const std::string &name) {
      const auto item = std::find_if(
          manifest.elements.begin(), manifest.elements.end(),
          [&](const auto &candidate) { return candidate.qualified_name == name; });
      require(item != manifest.elements.end(), "literal fixture was not extracted");
      const auto variant = std::find_if(
          manifest.variants.begin(), manifest.variants.end(),
          [&](const auto &candidate) {
            return candidate.element_id == item->element_id;
          });
      require(variant != manifest.variants.end(), "literal variant was not extracted");
      return variant->implementation_fingerprint;
    };
    require(implementation("enabled") != implementation("disabled"),
            "boolean literal values were not fingerprinted");
    require(implementation("low_ratio") != implementation("high_ratio"),
            "floating-point literal values were not fingerprinted");
    require(implementation("first_character") !=
                implementation("second_character"),
            "character literal values were not fingerprinted");
    require(!implementation("wide_text").empty(),
            "wide string literal was not fingerprinted");
    std::cout << "extractor entity tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
