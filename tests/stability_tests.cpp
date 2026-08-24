#include "history/ir.hpp"
#include "history/query.hpp"
#include "history/stability.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "catch_amalgamated.hpp"

namespace {
void require(bool condition, const char *message) {
  INFO(message);
  REQUIRE(condition);
}

history::EvidenceBundle bundle(std::string revision, int index) {
  history::EvidenceBundle result;
  result.source_revision = std::move(revision);
  result.configuration = "debug";
  history::ElementSnapshot stable;
  stable.compiler_id = "stable";
  stable.kind = "function";
  stable.qualified_name = "stable";
  stable.interface_fingerprint = "interface";
  stable.implementation_fingerprint = "implementation";
  stable.dependency_fingerprint = "dependency";
  stable.location.path = "stable/file.cpp";
  auto variable = stable;
  variable.compiler_id = "variable";
  variable.qualified_name = "variable";
  variable.location.path = "variable/file.cpp";
  variable.implementation_fingerprint = std::to_string(index);
  result.elements = {stable, variable};
  return result;
}
} // namespace

TEST_CASE("stability classification and explanations preserve evidence") {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-stability-" +
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
    nlohmann::json paths = nlohmann::json::array();
    for (int index = 0; index < 4; ++index) {
      const auto path = root / (std::to_string(index) + ".json");
      std::ofstream(path)
          << nlohmann::json(bundle("r" + std::to_string(index), index)).dump();
      paths.push_back(path.string());
    }
    history::BuildVariant cortex_r5;
    cortex_r5.product = "fixture";
    cortex_r5.target = "cortex-r5";
    cortex_r5.configuration = "debug";
    cortex_r5.variant_id = history::stable_hash(
        nlohmann::json({{"product", cortex_r5.product},
                        {"target", cortex_r5.target},
                        {"configuration", cortex_r5.configuration}})
            .dump());
    auto cortex_r7 = cortex_r5;
    cortex_r7.target = "cortex-r7";
    cortex_r7.variant_id = history::stable_hash(
        nlohmann::json({{"product", cortex_r7.product},
                        {"target", cortex_r7.target},
                        {"configuration", cortex_r7.configuration}})
            .dump());
    const auto manifest_path = root / "manifest.json";
    std::ofstream(manifest_path)
        << nlohmann::json(
               {{"schema_version", history::kSchemaVersion},
                {"series",
                 {{{"configuration", "debug"},
                   {"build_variant", cortex_r5},
                   {"translation_unit", "fixture.cpp"},
                   {"bundles", paths}},
                  {{"configuration", "debug"},
                   {"build_variant", cortex_r7},
                   {"translation_unit", "fixture.cpp"},
                   {"bundles", paths}}}},
                {"partition",
                 {{"stable", {"stable/*"}}, {"variable", {"variable/*"}}}},
                {"report", (root / "report.v1.json").string()}})
               .dump();
    const auto result = history::run_stability_experiment(manifest_path);
    require(result.at("classifications").value("stable", 0U) == 2,
            "stable element classification failed");
    require(result.at("classifications").value("variable", 0U) == 2,
            "variable element classification failed");
    require(result.at("cross_variant_facts").size() == 2 &&
                !result.at("cross_variant_facts")
                     .front()
                     .at("cross_variant_divergence")
                     .get<bool>(),
            "cross-variant facts were not separated from classification");
    require(std::filesystem::exists(root / "report.v1.csv"),
            "stability CSV was not written");
    history::QueryService service(std::make_shared<history::MemoryFactStore>());
    const auto explanation =
        service.execute({{"schema_version", history::kSchemaVersion},
                         {"query", "element.explain"},
                         {"params",
                          {{"report", (root / "report.v1.json").string()},
                           {"current_name", "variable"}}}});
    require(explanation.value("ok", false) &&
                explanation.at("result").at("matches").size() == 2,
            "element explanation query failed");
    const auto &explained = explanation.at("result").at("matches").front();
    require(explained.at("classification") == "variable" &&
                explained.at("historical_facts")
                        .value("implementation_changes", 0U) == 3,
            "element explanation omitted historical facts");
    require(explanation.at("result").at("policy").value("interface_weight",
                                                        0.0) == 4.0,
            "element explanation changed classifier policy");
    const auto partial_manifest = root / "partial-manifest.json";
    std::ofstream(partial_manifest)
        << nlohmann::json(
               {{"schema_version", history::kSchemaVersion},
                {"series",
                 {{{"configuration", "debug"},
                   {"build_variant", cortex_r5},
                   {"translation_unit", "fixture.cpp"},
                   {"coverage_complete", false},
                   {"bundles", paths}}}},
                {"partition",
                 {{"stable", {"stable/*"}}, {"variable", {"variable/*"}}}}})
               .dump();
    const auto partial =
        history::run_stability_experiment(partial_manifest);
    require(partial.at("classifications")
                .value("insufficient_evidence", 0U) == 2 &&
                partial.at("classifications").value("stable", 0U) == 0 &&
                partial.at("classifications").value("variable", 0U) == 0,
            "truncated progressive history reached a definitive classifier");
}
