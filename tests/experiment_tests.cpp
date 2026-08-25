#include "history/experiment.hpp"
#include "history/ir.hpp"
#include "history/process.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void write(const std::filesystem::path &path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
}

nlohmann::json progressive_budget() {
  nlohmann::json result;
  for (const auto &name :
       {"common_leakage", "variable_detail", "stable_island_candidates",
        "high_impact_headers", "controls"})
    result[name] = {{"max_files", 20},
                    {"max_syntax_transitions", 100},
                    {"max_semantic_elements", 100}};
  result["max_capture_revisions"] = 20;
  result["max_dependency_depth"] = 2;
  result["max_induced_elements_per_transition"] = 500;
  return result;
}

nlohmann::json first_manifest(const std::filesystem::path &directory) {
  for (const auto &entry : std::filesystem::directory_iterator(directory))
    if (entry.is_regular_file() && entry.path().extension() == ".json") {
      std::ifstream input(entry.path());
      return nlohmann::json::parse(input);
    }
  throw std::runtime_error("manifest directory is empty");
}

} // namespace

int main() {
  try {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("repotraverse-experiment-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup {
      std::filesystem::path path;
      ~Cleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
      }
    } cleanup{root};
    const auto repository = root / "repository";
    write(repository / "include/config.hpp", "#define VALUE 7\n");
    write(repository / "src/sample.cpp",
          "#include \"config.hpp\"\nint sample() { return VALUE; }\n");
    require(history::run_process({GIT_PATH, "init", "-b", "main"}, repository)
                    .exit_code == 0,
            "cannot initialize experiment repository");
    require(history::run_process(
                {GIT_PATH, "-C", repository.string(), "-c", "user.name=Test",
                 "-c", "user.email=test@example.invalid", "add", "."})
                    .exit_code == 0,
            "cannot stage experiment repository");
    require(history::run_process({GIT_PATH, "-C", repository.string(), "-c",
                                  "user.name=Test", "-c",
                                  "user.email=test@example.invalid", "commit",
                                  "-m", "base"})
                    .exit_code == 0,
            "cannot commit experiment repository");
    auto revision = history::run_process(
        {GIT_PATH, "-C", repository.string(), "rev-parse", "HEAD"});
    while (!revision.output.empty() &&
           (revision.output.back() == '\n' || revision.output.back() == '\r'))
      revision.output.pop_back();
    const auto experiment_manifest = root / "head.json";
    const nlohmann::json experiment = {
        {"schema_version", history::kSchemaVersion},
        {"repository", repository.string()},
        {"repository_id", "fixture"},
        {"revision", revision.output},
        {"output", (root / "head-output").string()},
        {"artifact_cache", (root / "artifact-cache-v1").string()},
        {"extractor", EXTRACTOR_PATH},
        {"configurations",
         nlohmann::json::array(
             {{{"name", "debug"},
               {"build_variant",
                {{"product", "fixture"},
                 {"target", "host"},
                 {"configuration", "debug"}}},
               {"toolchain", "armclang6"},
               {"real_compiler", FAKE_ARM_COMPILER_PATH},
               {"compiler_variables", nlohmann::json::array()},
               {"command", {FAKE_MAKE_PATH, "CC={compiler_probe}"}}}})}};
    write(experiment_manifest, experiment.dump());
    const auto head =
        history::run_head_experiment(experiment_manifest, PROBE_PATH);
    require(head.at("translation_unit_contexts") == 1,
            "HEAD experiment did not extract captured TU");
    require(head.at("extracted_elements").get<std::uint64_t>() > 0,
            "HEAD experiment did not extract elements");
    require(head.at("states").value("complete", 0U) == 1,
            "captured dependency map did not produce complete coverage");
    require(head.at("capture").at("import").at(
                "ignored_non_translation_unit_records") == 1,
            "non-TU compiler invocation was not accounted for");

    auto aliased_variant = experiment;
    aliased_variant["output"] = (root / "alias-head-output").string();
    aliased_variant["configurations"][0]["build_variant"]["product"] =
        "fixture-alias";
    const auto alias_manifest = root / "alias-head.json";
    write(alias_manifest, aliased_variant.dump());
    const auto alias_head =
        history::run_head_experiment(alias_manifest, PROBE_PATH);
    require(alias_head.at("cache_hits") == 1,
            "equivalent build variant did not share semantic artifact cache");
    require(first_manifest(std::filesystem::path(
                               alias_head.at("output").get<std::string>()) /
                           "manifests")
                    .at("build_variant")
                    .at("product") == "fixture-alias",
            "cached semantic artifact lost its build-variant observation");

    for (int index = 1; index <= 3; ++index) {
      write(repository / "README.md", "documentation " + std::to_string(index));
      require(history::run_process(
                  {GIT_PATH, "-C", repository.string(), "add", "README.md"})
                      .exit_code == 0,
              "cannot stage pilot revision");
      require(history::run_process({GIT_PATH, "-C", repository.string(), "-c",
                                    "user.name=Test", "-c",
                                    "user.email=test@example.invalid", "commit",
                                    "-m", "documentation"})
                      .exit_code == 0,
              "cannot commit pilot revision");
    }
    auto pilot_experiment = experiment;
    pilot_experiment["revision"] = "main";
    pilot_experiment["output"] = (root / "pilot-output").string();
    pilot_experiment["pilot"] = {{"ref", "main"},
                                 {"max_revisions", 4},
                                 {"budget", progressive_budget()}};
    auto missing_budget = pilot_experiment;
    missing_budget["output"] = (root / "missing-budget-output").string();
    missing_budget["pilot"].erase("budget");
    const auto missing_budget_manifest = root / "missing-budget.json";
    write(missing_budget_manifest, missing_budget.dump());
    bool budget_rejected = false;
    try {
      (void)history::run_pilot_experiment(missing_budget_manifest, PROBE_PATH);
    } catch (const std::exception &error) {
      budget_rejected = std::string(error.what()).find("explicit budget") !=
                        std::string::npos;
    }
    require(budget_rejected, "pilot accepted a manifest without explicit caps");
    const auto pilot_manifest = root / "pilot.json";
    write(pilot_manifest, pilot_experiment.dump());
    const auto pilot =
        history::run_pilot_experiment(pilot_manifest, PROBE_PATH);
    require(pilot.at("series").size() == 1 &&
                pilot.at("series").front().at("bundles").size() == 4,
            "pilot did not create an ordered four-revision series");
    require(pilot.at("revision_reports").back().at("cache_hits") == 1,
            "pilot did not reuse an unaffected TU manifest");
    require(pilot.at("revision_reports").back().at("capture").at("reused") ==
                true,
            "pilot did not reuse an unchanged build context");
    require(pilot.at("revision_reports")
                    .front()
                    .at("units")
                    .front()
                    .at("cache_source") == "content_addressed",
            "pilot did not reuse the content-addressed HEAD artifact");
    require(
        pilot.at("revision_reports").front().at("workspace").at("mode") ==
                "temporary_full" &&
            pilot.at("revision_reports").back().at("workspace").at("mode") ==
                "sparse",
        "pilot did not switch from capture workspace to sparse extraction");
    auto changed_pilot_experiment = pilot_experiment;
    changed_pilot_experiment["pilot"]["budget"]["max_dependency_depth"] = 3;
    write(pilot_manifest, changed_pilot_experiment.dump());
    const auto rerun =
        history::run_pilot_experiment(pilot_manifest, PROBE_PATH);
    require(rerun.at("pilot_analysis_identity") !=
                pilot.at("pilot_analysis_identity"),
            "changed pilot inputs retained the previous analysis identity");
    for (const auto &report : rerun.at("revision_reports"))
      require(report.at("pilot_analysis_identity") ==
                  rerun.at("pilot_analysis_identity"),
              "pilot reused a stale revision report after its inputs changed");

    const auto missing_manifest_directory =
        std::filesystem::path(rerun.at("revision_reports")
                                  .at(1)
                                  .at("output")
                                  .get<std::string>()) /
        "manifests";
    for (const auto &entry :
         std::filesystem::directory_iterator(missing_manifest_directory))
      if (entry.is_regular_file() && entry.path().extension() == ".json") {
        std::filesystem::remove(entry.path());
        break;
      }
    const auto incomplete =
        history::run_pilot_experiment(pilot_manifest, PROBE_PATH);
    require(
        !incomplete.at("series").front().at("coverage_complete") &&
            !incomplete.at("series").front().at("missing_revisions").empty(),
        "a missing semantic revision was presented as complete coverage");

    auto clean_experiment = experiment;
    clean_experiment["revision"] = "main";
    clean_experiment["output"] = (root / "clean-head-output").string();
    clean_experiment.erase("artifact_cache");
    const auto clean_manifest = root / "clean-head.json";
    write(clean_manifest, clean_experiment.dump());
    const auto clean = history::run_head_experiment(clean_manifest, PROBE_PATH);
    const auto incremental_manifest =
        first_manifest(std::filesystem::path(pilot.at("revision_reports")
                                                 .back()
                                                 .at("output")
                                                 .get<std::string>()) /
                       "manifests");
    const auto full_manifest = first_manifest(
        std::filesystem::path(clean.at("output").get<std::string>()) /
        "manifests");
    require(incremental_manifest == full_manifest,
            "incremental and clean full manifests diverged");

    std::cout << "experiment tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
